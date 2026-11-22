/// @file system_monitor_app.cpp
/// @brief Exact 1:1 Obsidian System Monitor Application on Nisaba GPU Engine.
/// Uses the standalone AppTheme (Light & Dark Modes) and Obsidian CSD Window Controls.

#include "system_monitor_app.hpp"
#include "history_graph_renderer.hpp"
#include "vector_icons.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <numbers>
#include <unistd.h>

namespace sysmon {

namespace {

constexpr const char* kPageTitles[6] = {
    "Processes",
    "CPU",
    "Memory",
    "Network",
    "Disk",
    "GPU"
};

struct TableColDef {
    const char* key;
    const char* title;
    float base_width;
};

// Exact column widths from src/views/processespage.cpp:
// kCpuWidth = 72, kMemoryWidth = 88, kIoWidth = 88, kPidWidth = 64 -> kNumbersWidth = 400
constexpr std::array<TableColDef, 6> kTableCols{{
    {"name",   "Name",   0.0f},
    {"cpu",    "CPU",    72.0f},
    {"memory", "Memory", 88.0f},
    {"read",   "Read",   88.0f},
    {"write",  "Write",  88.0f},
    {"pid",    "PID",    64.0f},
}};

std::vector<double> sum_series(const std::vector<double>& first, const std::vector<double>& second) {
    std::vector<double> sum = first;
    for (size_t i = 0; i < sum.size(); ++i) {
        sum[i] += (i < second.size()) ? second[i] : 0.0;
    }
    return sum;
}

} // namespace

SystemMonitorApp::SystemMonitorApp(Monitor& monitor, float width, float height, bool dark_mode)
    : monitor_(monitor)
    , width_(width)
    , height_(height)
    , dark_mode_(dark_mode) {
    scale_ = UnitScale::compute(width, height);
    boot_time_ = Monitor::systemBootTime();

    target_theme_ = dark_mode_ ? AppTheme::dark() : AppTheme::light();
    theme_ = target_theme_;

    // Connect live to Obsidian Theme Manager ($XDG_RUNTIME_DIR/obsidian-shell.sock)
    // Uses app's own Dark/Light palettes, and pulls Live Monet seed from Shell in "live" mode.
    theme_client_.start([this](const obsidian::ThemeEvent& ev) {
        std::lock_guard<std::mutex> lock(theme_ev_mutex_);
        pending_theme_ev_ = ev;
        theme_ev_dirty_.store(true, std::memory_order_release);
    });
    // Snap initial system theme & apps opacity immediately without startup flash
    apply_system_theme(theme_client_.snapshot());
    theme_ = target_theme_;
    window_opacity_ = target_window_opacity_;
    theme_ev_dirty_.store(false, std::memory_order_release);

    seg_plate_pos_ = static_cast<float>(monitor_.scope());
    selection_.application = (monitor_.scope() == 0);
}

SystemMonitorApp::~SystemMonitorApp() {
    theme_client_.stop();
}

void SystemMonitorApp::apply_system_theme(const obsidian::ThemeEvent& ev) {
    target_window_opacity_ = std::clamp(ev.apps_opacity, 0.0f, 1.0f);
    if (ev.is_light()) {
        dark_mode_ = false;
        target_theme_ = AppTheme::light();
    } else if (ev.is_live()) {
        dark_mode_ = true;
        uint8_t r = 80, g = 227, b = 194;
        obsidian::ThemeEvent::parse_hex_rgb(ev.live_seed, r, g, b);
        target_theme_ = AppTheme::live(r, g, b);
    } else {
        dark_mode_ = true;
        target_theme_ = AppTheme::dark();
    }
}

void SystemMonitorApp::set_window_and_platform(nisaba::backend_os::Window* win, nisaba::backend_os::Platform* plat) {
    window_ = win;
    platform_ = plat;
    csd_frame_.set_window_and_platform(win, plat);
}

void SystemMonitorApp::resize(float width, float height) {
    width_ = width;
    height_ = height;
    scale_ = UnitScale::compute(width, height);
}

void SystemMonitorApp::set_dark_mode(bool dark) {
    if (dark_mode_ == dark) return;
    dark_mode_ = dark;
    target_theme_ = dark_mode_ ? AppTheme::dark() : AppTheme::light();
}

void SystemMonitorApp::set_current_page(int page) {
    int max_page = monitor_.metrics().gpuAvailable ? 5 : 4;
    page = std::clamp(page, 0, max_page);
    if (current_page_ == page) return;
    current_page_ = page;
    res_scroll_y_ = 0.0f;
    res_target_scroll_y_ = 0.0f;
    res_scrollbar_opacity_ = 0.0f;
    res_scrollbar_timer_ = 0.0f;
    ctx_menu_open_ = false;
}

ProcessList& SystemMonitorApp::active_list() {
    return (monitor_.scope() == 0) ? monitor_.applications() : monitor_.processes();
}

const ProcessRecord* SystemMonitorApp::find_record(const SelectionTarget& target) const {
    if (target.pid <= 0) return nullptr;
    return target.application
        ? monitor_.application(target.pid, target.startTicks)
        : monitor_.process(target.pid, target.startTicks);
}

const ProcessRecord* SystemMonitorApp::selected_record() const {
    return find_record(selection_);
}

void SystemMonitorApp::select_row(size_t row) {
    ProcessList& lst = active_list();
    const ProcessRecord* proc = lst.at(row);
    selection_.application = (monitor_.scope() == 0);
    selection_.pid = proc ? proc->pid : 0;
    selection_.startTicks = proc ? proc->startTicks : 0;
}

void SystemMonitorApp::clear_selection() {
    selection_.pid = 0;
    selection_.startTicks = 0;
    selection_.application = (monitor_.scope() == 0);
}

void SystemMonitorApp::scroll_to_row(size_t row) {
    float row_h = 36.0f * scale_.u;
    float top_y = static_cast<float>(row) * row_h;
    float bot_y = top_y + row_h;
    float vis_h = rect_table_body_.height();
    if (vis_h <= 0.0f) return;

    if (top_y < proc_target_scroll_y_) {
        proc_target_scroll_y_ = top_y;
        proc_scrollbar_timer_ = 1.25f;
    } else if (bot_y > proc_target_scroll_y_ + vis_h) {
        proc_target_scroll_y_ = bot_y - vis_h;
        proc_scrollbar_timer_ = 1.25f;
    }
    proc_target_scroll_y_ = std::clamp(proc_target_scroll_y_, 0.0f, proc_max_scroll_y_);
}

void SystemMonitorApp::open_details_modal() {
    if (!selected_record()) return;
    details_target_ = selection_;
    details_members_scroll_y_ = 0.0f;
    details_open_ = true;
    ctx_menu_open_ = false;
}

void SystemMonitorApp::open_confirm_modal(bool force) {
    const ProcessRecord* proc = selected_record();
    if (!proc || !Monitor::canManage(*proc)) return;
    confirm_target_ = selection_;
    confirm_force_ = force;
    confirm_open_ = true;
    ctx_menu_open_ = false;
}

void SystemMonitorApp::show_toast(std::string msg) {
    toast_text_ = std::move(msg);
    toast_timer_ = 4.0f;
}

std::string SystemMonitorApp::format_running_time(const ProcessRecord& proc) const {
    const long ticks = sysconf(_SC_CLK_TCK);
    if (boot_time_ <= 0 || ticks <= 0) return "—";
    const int64_t started = boot_time_ + static_cast<int64_t>(proc.startTicks / static_cast<uint64_t>(ticks));
    const int64_t now = static_cast<int64_t>(std::time(nullptr));

    std::time_t st_time = static_cast<std::time_t>(started);
    std::time_t nw_time = static_cast<std::time_t>(now);
    std::tm st_tm{};
    std::tm nw_tm{};
    localtime_r(&st_time, &st_tm);
    localtime_r(&nw_time, &nw_tm);

    char time_buf[64];
    if (st_tm.tm_year == nw_tm.tm_year && st_tm.tm_yday == nw_tm.tm_yday) {
        std::snprintf(time_buf, sizeof(time_buf), "%02d:%02d", st_tm.tm_hour, st_tm.tm_min);
    } else {
        std::snprintf(time_buf, sizeof(time_buf), "%04d-%02d-%02d %02d:%02d",
                      st_tm.tm_year + 1900, st_tm.tm_mon + 1, st_tm.tm_mday,
                      st_tm.tm_hour, st_tm.tm_min);
    }
    return Monitor::formatDuration(now - started) + ", since " + time_buf;
}

void SystemMonitorApp::update(float dt) {
    monitor_.tick();

    // Drain live theme events from obsidian-shell.sock safely on the main GUI thread
    if (theme_ev_dirty_.exchange(false, std::memory_order_acq_rel)) {
        obsidian::ThemeEvent ev;
        {
            std::lock_guard<std::mutex> lock(theme_ev_mutex_);
            ev = pending_theme_ev_;
        }
        apply_system_theme(ev);
    }

    theme_ = AppTheme::lerp(theme_, target_theme_, std::min(1.0f, dt * 14.0f));
    window_opacity_ += (target_window_opacity_ - window_opacity_) * std::min(1.0f, dt * 12.0f);
    csd_frame_.update(dt);

    // SegmentedControl damped spring physics (src/ui/segmented_control.cpp: kGlideResponse = 0.3, kStretch = 0.08)
    const float target_seg = static_cast<float>(monitor_.scope());
    const float offset = seg_plate_pos_ - target_seg;
    if (std::abs(offset) > 1e-4f || std::abs(seg_plate_vel_) > 1e-3f) {
        constexpr float kGlideResponse = 0.30f;
        constexpr float kStretch = 0.08f;
        const float omega = 2.0f * std::numbers::pi_v<float> / kGlideResponse;
        const float coeff = seg_plate_vel_ + omega * offset;
        const float decay = std::exp(-omega * dt);
        const float next = (offset + coeff * dt) * decay;
        seg_plate_vel_ = (coeff - omega * (offset + coeff * dt)) * decay;
        if (std::abs(next) < 1e-3f && std::abs(seg_plate_vel_) < 1e-2f) {
            seg_plate_pos_ = target_seg;
            seg_plate_vel_ = 0.0f;
            seg_plate_stretch_ = 0.0f;
        } else {
            const float top_speed = omega / std::numbers::e_v<float>;
            seg_plate_pos_ = target_seg + next;
            seg_plate_stretch_ = kStretch * std::min(1.0f, std::abs(seg_plate_vel_) / top_speed);
        }
    } else {
        seg_plate_pos_ = target_seg;
        seg_plate_vel_ = 0.0f;
        seg_plate_stretch_ = 0.0f;
    }

    cursor_blink_ += dt;
    if (cursor_blink_ >= 1.0f) cursor_blink_ -= 1.0f;

    proc_scroll_y_ += (proc_target_scroll_y_ - proc_scroll_y_) * std::min(1.0f, dt * 20.0f);
    res_scroll_y_ += (res_target_scroll_y_ - res_scroll_y_) * std::min(1.0f, dt * 20.0f);

    // ScrollBar auto-fade logic (src/ui/scroll_bar.cpp: kFadeDuration = 250ms, kHideDelay = 1000ms)
    if (proc_scrollbar_timer_ > 0.0f) {
        proc_scrollbar_timer_ = std::max(0.0f, proc_scrollbar_timer_ - dt);
        proc_scrollbar_opacity_ = std::min(1.0f, proc_scrollbar_opacity_ + dt * 6.0f);
    } else {
        proc_scrollbar_opacity_ = std::max(0.0f, proc_scrollbar_opacity_ - dt * 4.0f);
    }

    if (res_scrollbar_timer_ > 0.0f) {
        res_scrollbar_timer_ = std::max(0.0f, res_scrollbar_timer_ - dt);
        res_scrollbar_opacity_ = std::min(1.0f, res_scrollbar_opacity_ + dt * 6.0f);
    } else {
        res_scrollbar_opacity_ = std::max(0.0f, res_scrollbar_opacity_ - dt * 4.0f);
    }

    if (toast_timer_ > 0.0f) {
        toast_timer_ = std::max(0.0f, toast_timer_ - dt);
    }
}

void SystemMonitorApp::render(nisaba::gpu::GpuCanvas& canvas) {
    const float u = scale_.u;
    // Exact layout metrics from src/views/mainwindow.cpp:
    // kSidebarWidth = 240, kHeaderHeight = 48, kHeaderBaseline = 28
    const float sidebar_w = 240.0f * u;
    const float header_h = 48.0f * u;
    const float header_baseline = 28.0f * u;

    // 1. Window Surface Background (synced live with Obsidian Apps Opacity slider)
    csd_frame_.render_frame_background(canvas, width_, height_, theme_, scale_, window_opacity_);

    // Clip all interior views to the window's rounded corners when not maximized
    canvas.save();
    if (!csd_frame_.is_maximized()) {
        float win_r = csd_frame_.get_corner_radius(theme_, scale_);
        canvas.clip_rrect(*nisaba::Rect::from_xywh(0.0f, 0.0f, width_, height_), nisaba::BorderRadius::all(win_r));
    }

    // 2. Left Resource Sidebar (x=0, y=48, w=240, h=height-48)
    render_sidebar(canvas, sidebar_w);

    // 3. Right Content Area:
    // In mainwindow.cpp:
    //   processes->setGeometry({kSidebarWidth, kHeaderBaseline, width - kSidebarWidth, height - kHeaderBaseline});
    //   pages->setGeometry({kSidebarWidth, kHeaderHeight, width - kSidebarWidth, height - kHeaderHeight});
    if (current_page_ == 0) {
        auto proc_rect = *nisaba::Rect::from_xywh(sidebar_w, header_baseline,
                                                  std::max(0.0f, width_ - sidebar_w),
                                                  std::max(0.0f, height_ - header_baseline));
        canvas.save();
        canvas.clip_rect(proc_rect);
        render_processes_page(canvas, proc_rect);
        canvas.restore();
    } else {
        auto res_rect = *nisaba::Rect::from_xywh(sidebar_w, header_h,
                                                 std::max(0.0f, width_ - sidebar_w),
                                                 std::max(0.0f, height_ - header_h));
        canvas.save();
        canvas.clip_rect(res_rect);
        render_resource_page(canvas, res_rect);
        canvas.restore();
    }

    // 4. Obsidian WindowHeader (48px height, top-left Close/Minimize/Maximize plates, Page Title at x=252)
    int pg = std::clamp(current_page_, 0, 5);
    csd_frame_.render_titlebar(canvas, width_, height_, theme_, scale_, kPageTitles[pg], 240.0f);

    // Subtle Light/Dark Mode Toggle Plate at top-left of WindowHeader (x = 12, y = 12, 22x22)
    {
        float btn_sz = 22.0f * u;
        rect_theme_toggle_ = *nisaba::Rect::from_xywh(12.0f * u, 12.0f * u, btn_sz, btn_sz);
        CsdFrame::paint_window_control(canvas, rect_theme_toggle_, "", theme_.darkMode,
                                       csd_frame_.is_focused(), true,
                                       theme_toggle_hovered_, theme_toggle_pressed_, u);
        bool strong = csd_frame_.is_focused() || theme_toggle_hovered_;
        nisaba::Color glyph_col = detail::with_alpha(
            theme_.darkMode ? detail::rgb_hex(0xFFFFFF) : detail::rgb_hex(0x0C0C0C),
            strong ? 0.85f : 0.45f
        );
        VectorIcons::draw_theme_toggle(canvas,
                                       rect_theme_toggle_.left() + btn_sz * 0.5f,
                                       rect_theme_toggle_.top() + btn_sz * 0.5f,
                                       6.0f * u, theme_.darkMode, glyph_col);
    }

    // 5. System Error Label (bottom center in accent::red)
    if (!monitor_.error().empty()) {
        float fsz = AppTheme::points(9.0f) * u;
        float ew = measure_text(canvas, monitor_.error(), fsz);
        canvas.draw_text((width_ - ew) * 0.5f, height_ - 12.0f * u, monitor_.error(),
                         nisaba::Paint(accent::red), fsz, "sans");
    }

    // 6. Overlays: Context Menu, Details Dialog, Confirm Dialog, Toast
    if (ctx_menu_open_) {
        render_context_menu(canvas);
    }
    if (details_open_) {
        render_details_modal(canvas);
    }
    if (confirm_open_) {
        render_confirm_modal(canvas);
    }
    if (toast_timer_ > 0.0f && !toast_text_.empty()) {
        render_toast(canvas);
    }

    canvas.restore();
}

void SystemMonitorApp::render_sidebar(nisaba::gpu::GpuCanvas& canvas, float sidebar_w) {
    const float u = scale_.u;
    const float header_h = 48.0f * u;
    // Exact margins & spacing from src/views/mainwindow.cpp:
    // column->setMargins({12, 4, 12, 16}); column->setSpacing(12);
    const float margin_x = 12.0f * u;
    const float card_w = sidebar_w - margin_x * 2.0f; // 216px at 1x
    const float spacing = 12.0f * u;
    float cy = header_h + 4.0f * u; // y = 52px at 1x

    sidebar_rects_.clear();
    const Metrics& metrics = monitor_.metrics();
    const History& history = monitor_.history();
    const bool ready = monitor_.ready();
    const int64_t duration_ms = static_cast<int64_t>(monitor_.historyDuration()) * 1000;

    // Helper for SidebarButton::paint (src/views/summarycard.cpp:34-43):
    // radius = theme.mediumRadius (10), fill = selected ? accent.withAlpha(darkMode ? 0.15 : 0.09) : theme.secondBackground
    // border = selected ? 1px in accent.withAlpha(0.5) : none
    auto draw_sidebar_plate = [&](const nisaba::Rect& r, nisaba::Color accent_col, bool selected) {
        float rad = theme_.mediumRadius * u;
        nisaba::Color fill = selected
            ? detail::with_alpha(accent_col, theme_.darkMode ? 0.15f : 0.09f)
            : theme_.secondBackground;
        canvas.fill_round_rect(r, rad, rad, nisaba::Paint(fill));
        if (selected) {
            canvas.stroke_round_rect(r, rad, rad,
                                     nisaba::Paint(detail::with_alpha(accent_col, 0.50f)),
                                     nisaba::Stroke{1.0f});
        }
    };

    // ── 1. ProcessesButton (sizeHint: {0, 36}, accentColor: theme.highlight) ──
    {
        float h = 36.0f * u;
        auto r = *nisaba::Rect::from_xywh(margin_x, cy, card_w, h);
        sidebar_rects_.push_back(r);

        bool sel = (current_page_ == 0);
        draw_sidebar_plate(r, theme_.highlight, sel);

        // Icon: kPadding = 12, kIcon = 16, color = isSelected() ? theme.highlight : theme.text
        float pad = 12.0f * u;
        float icon_r = 8.0f * u;
        VectorIcons::draw_processes(canvas, r.left() + pad + icon_r, r.top() + h * 0.5f, icon_r,
                                    sel ? theme_.highlight : theme_.text);

        // Title: "Processes", 9pt (12px), weight 500, x = kPadding + kIcon + 8 = 36
        float title_fsz = AppTheme::points(9.0f) * u;
        canvas.draw_text(r.left() + 36.0f * u, r.top() + 22.5f * u, "Processes",
                         nisaba::Paint(theme_.text), title_fsz, "sans");

        // Count: 9pt (12px), disabledText, right-aligned at right - kPadding
        if (ready) {
            std::string cnt = std::to_string(metrics.processCount);
            float cw = measure_text(canvas, cnt, title_fsz);
            canvas.draw_text(r.right() - pad - cw, r.top() + 22.5f * u, cnt,
                             nisaba::Paint(theme_.disabledText), title_fsz, "sans");
        }

        cy += h + spacing;
    }

    // ── 2. Resource SummaryCards (sizeHint: {0, 96}) ────────────────────────
    // CPU: accent::blue, Memory: accent::purple, Network: accent::green, Disk: accent::orange, GPU: accent::pink
    struct CardSpec {
        int page;
        const char* title;
        nisaba::Color accent;
        std::string value;
        std::string subtitle;
        std::vector<double> series;
        double fixed_max;
        double scale_floor;
    };

    std::string cores_sub = ready
        ? (std::to_string(metrics.cores.size()) + (metrics.cores.size() == 1 ? " logical processor" : " logical processors"))
        : "Collecting data…";
    std::string mem_sub = ready
        ? (Monitor::formatBytes(static_cast<double>(metrics.memory.used)) + " / " + Monitor::formatBytes(static_cast<double>(metrics.memory.total)))
        : "Collecting data…";
    std::string net_sub = ready
        ? ("↑ " + Monitor::formatRate(metrics.upload))
        : "Collecting data…";
    std::string disk_sub = ready
        ? ("Write " + Monitor::formatRate(metrics.diskWrite))
        : "Collecting data…";

    std::vector<CardSpec> specs = {
        {1, "CPU",     accent::blue,   ready ? Monitor::formatPercent(metrics.cpuPercent) : "—",          cores_sub, history.cpuPercent,    100.0, 1024.0},
        {2, "Memory",  accent::purple, ready ? Monitor::formatPercent(metrics.memory.percent) : "—",      mem_sub,   history.memoryPercent, 100.0, 1024.0},
        {3, "Network", accent::green,  ready ? ("↓ " + Monitor::formatRate(metrics.download)) : "—",     net_sub,   sum_series(history.download, history.upload), 0.0, 1024.0 * 1024.0},
        {4, "Disk",    accent::orange, ready ? Monitor::formatRate(metrics.diskRead) : "—",               disk_sub,  sum_series(history.diskRead, history.diskWrite), 0.0, 1024.0 * 1024.0},
    };

    if (metrics.gpuAvailable) {
        std::string gpu_val = ready
            ? (metrics.gpuHasBusy ? Monitor::formatPercent(metrics.gpuPercent) : Monitor::formatTemperature(metrics.gpuTemperature))
            : "—";
        std::string gpu_sub = ready
            ? ((metrics.gpuHasBusy && metrics.gpuHasTemperature) ? Monitor::formatTemperature(metrics.gpuTemperature) : metrics.gpuName)
            : "Collecting data…";
        specs.push_back({5, "GPU", accent::pink, gpu_val, gpu_sub,
                         metrics.gpuHasBusy ? history.gpuPercent : history.gpuTemperature, 100.0, 1024.0});
    }

    const float card_h = 96.0f * u; // Exact SummaryCard::sizeHint() = {0, 96}
    const float pad = 12.0f * u;
    const float title_fsz = AppTheme::points(9.0f) * u; // 12px
    const float sub_fsz   = AppTheme::points(8.0f) * u; // 10.67px

    for (const auto& sp : specs) {
        auto r = *nisaba::Rect::from_xywh(margin_x, cy, card_w, card_h);
        sidebar_rects_.push_back(r);

        bool sel = (current_page_ == sp.page);
        draw_sidebar_plate(r, sp.accent, sel);

        // SummaryCard::layout() (src/views/summarycard.cpp:162-188):
        // content = localRect().adjusted(12, 12, -12, -12)
        // icon (16x16) at content.x, content.y
        float icon_cx = r.left() + pad + 8.0f * u;
        float icon_cy = r.top() + pad + 6.0f * u;

        switch (sp.page) {
            case 1: VectorIcons::draw_cpu(canvas, icon_cx, icon_cy, 8.0f * u, sp.accent); break;
            case 2: VectorIcons::draw_memory(canvas, icon_cx, icon_cy, 8.0f * u, sp.accent); break;
            case 3: VectorIcons::draw_network(canvas, icon_cx, icon_cy, 8.0f * u, sp.accent); break;
            case 4: VectorIcons::draw_disk(canvas, icon_cx, icon_cy, 8.0f * u, sp.accent); break;
            case 5: VectorIcons::draw_gpu(canvas, icon_cx, icon_cy, 8.0f * u, sp.accent); break;
            default: break;
        }

        // Title at content.x + kIcon + 8 (x + 36), baseline = content.y + capHeight (~10px)
        float title_x = r.left() + pad + 24.0f * u;
        float row1_baseline = r.top() + pad + 10.0f * u;
        canvas.draw_text(title_x, row1_baseline, sp.title, nisaba::Paint(theme_.text), title_fsz, "sans");

        // Value at content.right() - valueWidth
        float vw = measure_text(canvas, sp.value, title_fsz);
        canvas.draw_text(r.right() - pad - vw, row1_baseline, sp.value, nisaba::Paint(theme_.text), title_fsz, "sans");

        // Subtitle at title_x, row + 5 (content.y + 19..28), in theme.disabledText
        float sub_baseline = r.top() + pad + 28.0f * u;
        std::string sub_elided = elide_right(canvas, sp.subtitle, r.width() - pad * 2.0f - 24.0f * u, sub_fsz);
        canvas.draw_text(title_x, sub_baseline, sub_elided, nisaba::Paint(theme_.disabledText), sub_fsz, "sans");

        // HistoryGraph at content.x, content.y + graphTop (~37px), height = content.height - graphTop (~35px)
        // LineWidth = 1.25, Curved = true
        float graph_top = r.top() + pad + 37.0f * u;
        float graph_h = std::max(10.0f * u, r.bottom() - pad - graph_top);
        auto graph_rect = *nisaba::Rect::from_xywh(r.left() + pad, graph_top, r.width() - pad * 2.0f, graph_h);

        HistoryGraphRenderer::render(
            canvas,
            graph_rect,
            sp.series,
            {},
            history.timestamps,
            duration_ms,
            sp.accent,
            sp.accent,
            std::nullopt,
            sp.fixed_max,
            sp.scale_floor,
            true,
            1.25f * u
        );

        cy += card_h + spacing;
    }
}

void SystemMonitorApp::render_processes_page(nisaba::gpu::GpuCanvas& canvas, const nisaba::Rect& page_rect) {
    const float u = scale_.u;
    // Exact geometry from src/views/processespage.cpp:
    // page_rect starts at (x = 240, y = 28)
    // kMargin = 12, kTop = 22, kSpacing = 16, kScopeWidth = 357, kRowHeight = 36, kFieldHeight = 30, kHeaderHeight = 30
    const float margin = 12.0f * u;
    const float top_y = page_rect.top() + 22.0f * u; // y = 28 + 22 = 50px
    const float spacing = 16.0f * u;
    const float left_x = page_rect.left() + margin; // x = 252px
    const float right_x = page_rect.right() - margin;
    const float avail_w = std::max(100.0f * u, page_rect.width() - 2.0f * margin);

    const float scope_w = std::min(357.0f * u, avail_w * 0.52f);
    const float scope_h = 36.0f * u;
    const float field_h = 30.0f * u;
    const float field_top = top_y + (scope_h - field_h) * 0.5f; // y = 53px

    // ── 1. SegmentedControl ("Applications", "My Processes", "All Processes") ──
    // Track: radius = height / 2 (18), color = MonitorColor::MutedPlate
    // Plate: inset = 2 + 2 = 4px from outer bounds (28px height, radius 14), color = theme.segmentedControlPlate,
    //        shadow = (0, 1) sigma 1.2 in theme.segmentedControlPlateShadow
    rect_scope_bar_ = *nisaba::Rect::from_xywh(left_x, top_y, scope_w, scope_h);
    canvas.fill_round_rect(rect_scope_bar_, scope_h * 0.5f, scope_h * 0.5f, nisaba::Paint(theme_.mutedPlate));

    const float seg_pad = 2.0f * u;
    const float plate_inset = 2.0f * u;
    const float inner_x = left_x + seg_pad;
    const float inner_y = top_y + seg_pad;
    const float inner_w = scope_w - 2.0f * seg_pad;
    const float inner_h = scope_h - 2.0f * seg_pad;
    const float item_w = inner_w / 3.0f;

    // Animated sliding plate
    {
        const float grown_w = item_w * (1.0f + seg_plate_stretch_);
        const float px = std::clamp(inner_x + item_w * seg_plate_pos_ - (grown_w - item_w) * 0.5f,
                                    inner_x, inner_x + inner_w - grown_w);
        auto plate_rect = *nisaba::Rect::from_xywh(px + plate_inset, inner_y + plate_inset,
                                                   grown_w - 2.0f * plate_inset, inner_h - 2.0f * plate_inset);
        float pr = plate_rect.height() * 0.5f;

        // Drop shadow under active pill
        auto sh_rect = *nisaba::Rect::from_xywh(plate_rect.left(), plate_rect.top() + 1.0f * u,
                                                plate_rect.width(), plate_rect.height());
        canvas.fill_round_rect(sh_rect, pr, pr, nisaba::Paint(theme_.segmentedControlPlateShadow));
        canvas.fill_round_rect(plate_rect, pr, pr, nisaba::Paint(theme_.segmentedControlPlate));
    }

    static constexpr const char* kScopes[3] = {"Applications", "My Processes", "All Processes"};
    const float seg_fsz = AppTheme::points(9.0f) * u; // 12px
    for (int i = 0; i < 3; ++i) {
        auto sr = *nisaba::Rect::from_xywh(inner_x + static_cast<float>(i) * item_w, inner_y, item_w, inner_h);
        rect_scope_segs_[i] = sr;

        nisaba::Color lbl_col = (pressed_scope_seg_ == i)
            ? theme_.segmentedControlTextPressed
            : theme_.text;
        float tw = measure_text(canvas, kScopes[i], seg_fsz);
        canvas.draw_text(sr.left() + (sr.width() - tw) * 0.5f, sr.top() + 20.0f * u,
                         kScopes[i], nisaba::Paint(lbl_col), seg_fsz, "sans");
    }

    // ── 2. QuitButton (right - kFieldHeight, fieldTop, 30x30) ──────────────
    const ProcessRecord* sel_proc = selected_record();
    const bool can_quit = (sel_proc && Monitor::canManage(*sel_proc));

    rect_quit_btn_ = *nisaba::Rect::from_xywh(right_x - field_h, field_top, field_h, field_h);
    {
        // QuitButton::paint (src/views/processespage.cpp:104-115):
        // fill = !enabled ? MonitorColor::MutedPlate
        //      : pressed ? tint(secondBackground, text.withAlpha(0.06))
        //      : hovered ? tint(secondBackground, text.withAlpha(0.025))
        //      : secondBackground
        nisaba::Color fill = theme_.secondBackground;
        if (!can_quit) {
            fill = theme_.mutedPlate;
        } else if (quit_btn_pressed_) {
            fill = detail::tint_color(theme_.secondBackground, detail::with_alpha(theme_.text, 0.06f));
        } else if (quit_btn_hovered_) {
            fill = detail::tint_color(theme_.secondBackground, detail::with_alpha(theme_.text, 0.025f));
        }
        canvas.fill_round_rect(rect_quit_btn_, field_h * 0.5f, field_h * 0.5f, nisaba::Paint(fill));

        // Quit icon: 16x16 centered, color = theme.text, opacity = isEnabled() ? 1.0 : 0.35
        nisaba::Color icon_col = detail::with_alpha(theme_.text, can_quit ? 1.0f : 0.35f);
        VectorIcons::draw_quit(canvas,
                               rect_quit_btn_.left() + field_h * 0.5f,
                               rect_quit_btn_.top() + field_h * 0.5f,
                               8.0f * u, icon_col);
    }

    // ── 3. SearchField (searchX, fieldTop, searchWidth, 30) ────────────────
    const float search_x = left_x + scope_w + spacing;
    const float search_w = std::max(0.0f, right_x - field_h - spacing - search_x);
    rect_search_box_ = *nisaba::Rect::from_xywh(search_x, field_top, search_w, field_h);

    // SearchField::paint (src/ui/search_field.cpp:77-91):
    // radius = height / 2 (15), fill = theme.secondBackground, border = 1px in (focused ? theme.highlight : fill)
    // focus ring = 2px outside in ColorId::TextFieldFocusRing
    {
        const float rad = field_h * 0.5f;
        if (search_focused_) {
            float fr = 2.0f * u;
            auto ring_rect = *nisaba::Rect::from_xywh(rect_search_box_.left() - fr, rect_search_box_.top() - fr,
                                                      rect_search_box_.width() + 2.0f * fr, rect_search_box_.height() + 2.0f * fr);
            canvas.stroke_round_rect(ring_rect, rad + fr, rad + fr,
                                     nisaba::Paint(theme_.textFieldFocusRing), nisaba::Stroke{2.0f * u});
        }
        canvas.fill_round_rect(rect_search_box_, rad, rad, nisaba::Paint(theme_.secondBackground));
        if (search_focused_) {
            canvas.stroke_round_rect(rect_search_box_, rad, rad,
                                     nisaba::Paint(theme_.highlight), nisaba::Stroke{1.0f});
        }

        // SearchField::layout (m_inset = 12, kGlyphSize = 14, m_gap = 6, m_hitMargin = 4)
        const float inset = 12.0f * u;
        const float glyph_sz = 14.0f * u;
        const float gap = 6.0f * u;
        const float hit_margin = 4.0f * u;

        VectorIcons::draw_search(canvas,
                                 rect_search_box_.left() + inset + glyph_sz * 0.5f,
                                 rect_search_box_.top() + field_h * 0.5f,
                                 glyph_sz * 0.5f, theme_.disabledText);

        rect_search_clear_ = *nisaba::Rect::from_xywh(
            rect_search_box_.right() - inset - glyph_sz - hit_margin,
            rect_search_box_.top() + (field_h - glyph_sz) * 0.5f - hit_margin,
            glyph_sz + 2.0f * hit_margin,
            glyph_sz + 2.0f * hit_margin
        );

        const std::string& query = monitor_.processes().search();
        const float text_x = rect_search_box_.left() + inset + glyph_sz + gap; // x + 32px
        const float search_fsz = AppTheme::points(9.0f) * u; // 12px

        if (query.empty()) {
            canvas.draw_text(text_x, rect_search_box_.top() + 19.5f * u, "Search processes",
                             nisaba::Paint(theme_.textFieldPlaceholder), search_fsz, "sans");
            if (search_focused_ && cursor_blink_ < 0.5f) {
                canvas.stroke_line(text_x, rect_search_box_.top() + 7.0f * u,
                                   text_x, rect_search_box_.bottom() - 7.0f * u,
                                   nisaba::Paint(theme_.text), nisaba::Stroke{1.2f});
            }
        } else {
            float max_tw = std::max(10.0f, search_w - 2.0f * (inset + glyph_sz + gap));
            std::string disp = elide_right(canvas, query, max_tw, search_fsz);
            canvas.draw_text(text_x, rect_search_box_.top() + 19.5f * u, disp,
                             nisaba::Paint(theme_.text), search_fsz, "sans");
            if (search_focused_ && cursor_blink_ < 0.5f) {
                float cw = measure_text(canvas, disp, search_fsz);
                canvas.stroke_line(text_x + cw + 1.0f * u, rect_search_box_.top() + 7.0f * u,
                                   text_x + cw + 1.0f * u, rect_search_box_.bottom() - 7.0f * u,
                                   nisaba::Paint(theme_.text), nisaba::Stroke{1.2f});
            }

            // Clear button icon (data/icons/clear.svg)
            VectorIcons::draw_clear(canvas,
                                    rect_search_box_.right() - inset - glyph_sz * 0.5f,
                                    rect_search_box_.top() + field_h * 0.5f,
                                    glyph_sz * 0.5f,
                                    theme_.disabledText,
                                    theme_.secondBackground);
        }
    }

    // ── 4. Process Table Plate (kMargin, tableTop, width - 2*kMargin, height - tableTop - kMargin) ──
    const float table_top = top_y + scope_h + spacing; // y = 50 + 36 + 16 = 102px
    const float table_h = std::max(60.0f * u, page_rect.bottom() - margin - table_top);
    rect_table_ = *nisaba::Rect::from_xywh(left_x, table_top, avail_w, table_h);

    const float table_rad = theme_.bigRadius * u; // 12px
    canvas.fill_round_rect(rect_table_, table_rad, table_rad, nisaba::Paint(theme_.secondBackground));

    // Column Headers (kHeaderHeight = 30, font size = points(8) = 10.67px, weight 500, color = disabledText)
    // Numeric titles at geometry().width - size.width - 12, indicator (10x10) at x + size.width + 3
    const float hdr_h = 30.0f * u;
    const float numbers_w = (72.0f + 88.0f + 88.0f + 88.0f + 64.0f) * u; // 400px
    const float name_w = std::max(80.0f * u, avail_w - numbers_w);
    const float hdr_fsz = AppTheme::points(8.0f) * u; // 10.67px

    const std::string& sort_col = monitor_.processes().sortColumn();
    const bool sort_asc = monitor_.processes().ascending();

    float col_x = left_x;
    for (size_t i = 0; i < kTableCols.size(); ++i) {
        float cw = (i == 0) ? name_w : (kTableCols[i].base_width * u);
        auto cr = *nisaba::Rect::from_xywh(col_x, table_top, cw, hdr_h);
        rect_col_headers_[i] = cr;

        const bool numeric = (i > 0);
        const float tw = measure_text(canvas, kTableCols[i].title, hdr_fsz);
        const float tx = numeric ? (cr.right() - tw - 12.0f * u) : (cr.left() + 12.0f * u);

        canvas.draw_text(tx, cr.top() + 19.0f * u, kTableCols[i].title,
                         nisaba::Paint(theme_.disabledText), hdr_fsz, "sans");

        if (sort_col == kTableCols[i].key) {
            // Indicator sits 3px after label (in the cell's 12px right margin for numeric columns)
            float ind_r = 5.0f * u;
            float ind_cx = tx + tw + 3.0f * u + ind_r;
            float ind_cy = cr.top() + hdr_h * 0.5f;
            VectorIcons::draw_chevron(canvas, ind_cx, ind_cy, ind_r, sort_asc, theme_.disabledText);
        }

        col_x += cw;
    }

    // Hairline separator below column headers: theme.text.withAlpha(0.07)
    canvas.stroke_line(left_x, table_top + hdr_h, left_x + avail_w, table_top + hdr_h,
                       nisaba::Paint(theme_.headerSeparator), nisaba::Stroke{1.0f});

    // ── 5. Process Rows (kRowHeight = 36, font size = points(9) = 12px) ────
    rect_table_body_ = *nisaba::Rect::from_xywh(left_x, table_top + hdr_h + 1.0f,
                                                avail_w, std::max(0.0f, table_h - hdr_h - 1.0f));

    ProcessList& lst = active_list();
    const size_t row_count = lst.count();
    const float row_h = 36.0f * u;
    const float total_rows_h = static_cast<float>(row_count) * row_h;
    proc_max_scroll_y_ = std::max(0.0f, total_rows_h - rect_table_body_.height());
    proc_target_scroll_y_ = std::clamp(proc_target_scroll_y_, 0.0f, proc_max_scroll_y_);
    proc_scroll_y_ = std::clamp(proc_scroll_y_, 0.0f, proc_max_scroll_y_);

    canvas.save();
    canvas.clip_rrect(rect_table_, nisaba::BorderRadius::all(table_rad));
    canvas.clip_rect(rect_table_body_);

    const float row_fsz = AppTheme::points(9.0f) * u; // 12px

    if (row_count == 0 && monitor_.ready()) {
        std::string empty_title = (monitor_.scope() == 0) ? "No applications found" : "No processes found";
        std::string empty_sub = "Try a different name, user, or process ID.";
        float tw1 = measure_text(canvas, empty_title, row_fsz);
        float tw2 = measure_text(canvas, empty_sub, row_fsz);
        float mid_y = rect_table_body_.top() + rect_table_body_.height() * 0.45f;
        canvas.draw_text(left_x + (avail_w - tw1) * 0.5f, mid_y, empty_title,
                         nisaba::Paint(theme_.text), row_fsz, "sans");
        canvas.draw_text(left_x + (avail_w - tw2) * 0.5f, mid_y + 20.0f * u, empty_sub,
                         nisaba::Paint(theme_.disabledText), row_fsz, "sans");
    } else {
        size_t first_vis = static_cast<size_t>(std::max(0.0f, std::floor(proc_scroll_y_ / row_h)));
        size_t last_vis = std::min(row_count,
                                   static_cast<size_t>(std::ceil((proc_scroll_y_ + rect_table_body_.height()) / row_h)) + 1);

        const bool app_mode = (monitor_.scope() == 0);
        // Alternating odd row tint from src/views/processespage.cpp:296: theme.text.withAlpha(0.018)
        const nisaba::Color odd_row_tint = detail::with_alpha(theme_.text, 0.018f);

        for (size_t row = first_vis; row < last_vis; ++row) {
            const ProcessRecord* proc = lst.at(row);
            if (!proc) continue;

            float ry = rect_table_body_.top() + static_cast<float>(row) * row_h - proc_scroll_y_;
            auto rr = *nisaba::Rect::from_xywh(left_x, ry, avail_w, row_h);

            const bool highlighted = (selection_.pid == proc->pid &&
                                      selection_.startTicks == proc->startTicks &&
                                      selection_.application == app_mode);

            if (highlighted) {
                // Solid theme.highlight (#3385FF) fill across row, clipped to table's rounded bottom corners
                canvas.fill_rect(rr, nisaba::Paint(theme_.highlight));
            } else if (row % 2 == 1) {
                canvas.fill_rect(rr, nisaba::Paint(odd_row_tint));
            }

            // Icon: 22x22 at x = 12, vertically centered
            VectorIcons::draw_process_badge(canvas, left_x + 23.0f * u, ry + row_h * 0.5f, 22.0f * u,
                                            proc->iconName, proc->displayName, theme_, highlighted);

            // Name: x = 44, width = nameWidth - 44 - 12, color = highlighted ? highlightedText : text
            std::string name_str = elide_right(canvas, proc->displayName, std::max(0.0f, name_w - 56.0f * u), row_fsz);
            canvas.draw_text(left_x + 44.0f * u, ry + 22.5f * u, name_str,
                             nisaba::Paint(highlighted ? theme_.highlightedText : theme_.text), row_fsz, "sans");

            // Numeric columns: CPU (0), Memory (1) in theme.text; Read (2), Write (3), PID (4) in theme.disabledText
            // Right-aligned at x + width - 12
            std::string vals[5] = {
                Monitor::formatPercent(proc->cpu),
                Monitor::formatBytes(static_cast<double>(proc->memory)),
                proc->ioAvailable ? Monitor::formatRate(proc->readRate) : "—",
                proc->ioAvailable ? Monitor::formatRate(proc->writeRate) : "—",
                std::to_string(proc->pid)
            };

            float vx = left_x + name_w;
            for (int c = 0; c < 5; ++c) {
                float cw = kTableCols[static_cast<size_t>(c + 1)].base_width * u;
                nisaba::Color normal_col = (c >= 2) ? theme_.disabledText : theme_.text;
                nisaba::Color vc = highlighted ? theme_.highlightedText : normal_col;
                float vw = measure_text(canvas, vals[c], row_fsz);
                canvas.draw_text(vx + cw - 12.0f * u - vw, ry + 22.5f * u, vals[c],
                                 nisaba::Paint(vc), row_fsz, "sans");
                vx += cw;
            }
        }
    }

    canvas.restore();

    // Scrollbar at the window's right edge (beside the table, fading in on scroll)
    if (proc_max_scroll_y_ > 0.0f && proc_scrollbar_opacity_ > 0.01f) {
        float track_h = rect_table_body_.height() - 8.0f * u;
        float thumb_h = std::min(track_h, std::max(32.0f * u, track_h * (rect_table_body_.height() / total_rows_h)));
        float thumb_y = rect_table_body_.top() + 4.0f * u + (track_h - thumb_h) * (proc_scroll_y_ / proc_max_scroll_y_);
        auto sbar = *nisaba::Rect::from_xywh(page_rect.right() - 7.0f * u, thumb_y, 4.0f * u, thumb_h);
        auto tu = theme_.scrollBarThumb.to_color_u8();
        uint8_t sa = static_cast<uint8_t>(static_cast<float>(tu.a) * proc_scrollbar_opacity_);
        canvas.fill_round_rect(sbar, 2.0f * u, 2.0f * u,
                               nisaba::Paint(nisaba::Color::from_rgba8(tu.r, tu.g, tu.b, sa)));
    }
}

void SystemMonitorApp::render_resource_page(nisaba::gpu::GpuCanvas& canvas, const nisaba::Rect& page_rect) {
    const float u = scale_.u;
    // Exact metrics from src/views/resourcepage.cpp:
    // m_content->setMargins({12, 8, 12, 24}); m_layout->setSpacing(20);
    const float margin_x = 12.0f * u;
    const float pad = 16.0f * u; // kPadding = 16 in ChartCard and InfoRow
    const float spacing = 20.0f * u;
    const float left_x = page_rect.left() + margin_x; // x = 252px
    const float avail_w = std::max(100.0f * u, page_rect.width() - 2.0f * margin_x);
    const float start_y = page_rect.top() + 8.0f * u - res_scroll_y_;
    float cy = start_y;

    const Metrics& metrics = monitor_.metrics();
    const History& history = monitor_.history();
    const int64_t duration_ms = static_cast<int64_t>(monitor_.historyDuration()) * 1000;
    const float card_rad = theme_.bigRadius * u; // 12px

    // ── 1. ChartCard (sizeHint: {0, 276}, Plate(theme.bigRadius)) ──────────
    auto draw_chart_card = [&](const std::string& title, const std::string& value,
                               const std::string& subtitle,
                               const std::vector<double>& primary,
                               const std::vector<double>& secondary,
                               nisaba::Color pri_col, nisaba::Color sec_col,
                               double fixed_max, bool temp_mode) {
        const float chart_card_h = 276.0f * u;
        auto card_rect = *nisaba::Rect::from_xywh(left_x, cy, avail_w, chart_card_h);
        canvas.fill_round_rect(card_rect, card_rad, card_rad, nisaba::Paint(theme_.secondBackground));

        // ChartCard::ChartCard() (src/views/chartcard.cpp):
        // m_title: 11pt (14.67px), weight 500, theme.text
        // m_value: 18pt (24px), weight 500, theme.text
        // m_subtitle: 9pt (12px), theme.disabledText
        // m_scale: 8pt (10.67px), theme.disabledText, right-aligned
        const float fsz_title = AppTheme::points(11.0f) * u;
        const float fsz_value = AppTheme::points(18.0f) * u;
        const float fsz_sub   = AppTheme::points(9.0f) * u;
        const float fsz_small = AppTheme::points(8.0f) * u;

        canvas.draw_text(left_x + pad, cy + pad + 14.0f * u, title,
                         nisaba::Paint(theme_.text), fsz_title, "sans");

        float vw = measure_text(canvas, value, fsz_value);
        canvas.draw_text(card_rect.right() - pad - vw, cy + pad + 20.0f * u, value,
                         nisaba::Paint(theme_.text), fsz_value, "sans");

        canvas.draw_text(left_x + pad, cy + pad + 36.0f * u, subtitle,
                         nisaba::Paint(theme_.disabledText), fsz_sub, "sans");

        double eff_max = HistoryGraphRenderer::effective_maximum(primary, secondary, fixed_max, 1024.0);
        std::string scale_lbl = temp_mode
            ? Monitor::formatTemperature(100)
            : (fixed_max > 0.0 ? "100%" : Monitor::formatRate(eff_max));
        float sw = measure_text(canvas, scale_lbl, fsz_small);
        canvas.draw_text(card_rect.right() - pad - sw, cy + pad + 52.0f * u, scale_lbl,
                         nisaba::Paint(theme_.disabledText), fsz_small, "sans");

        // HistoryGraph: gridColor = theme.text.withAlpha(0.06), lineWidth = 1.5, curved = false
        float graph_top = cy + pad + 62.0f * u;
        float graph_h = chart_card_h - pad * 2.0f - 84.0f * u;
        auto graph_rect = *nisaba::Rect::from_xywh(left_x + pad, graph_top, avail_w - pad * 2.0f, graph_h);

        HistoryGraphRenderer::render(
            canvas,
            graph_rect,
            primary,
            secondary,
            history.timestamps,
            duration_ms,
            pri_col,
            sec_col,
            detail::with_alpha(theme_.text, 0.06f),
            fixed_max,
            1024.0,
            false,
            1.5f * u
        );

        // Bottom row: m_duration ("60 seconds"), m_primaryLegend, m_secondaryLegend, m_now ("Now")
        float bot_y = card_rect.bottom() - pad;
        std::string dur_txt = std::to_string(monitor_.historyDuration()) + " seconds";
        canvas.draw_text(left_x + pad, bot_y, dur_txt, nisaba::Paint(theme_.disabledText), fsz_small, "sans");

        float rx = card_rect.right() - pad;
        float now_w = measure_text(canvas, "Now", fsz_small);
        rx -= now_w;
        canvas.draw_text(rx, bot_y, "Now", nisaba::Paint(theme_.disabledText), fsz_small, "sans");

        if (current_page_ == 3 || current_page_ == 4) {
            std::string leg1 = (current_page_ == 3) ? "Download" : "Read";
            std::string leg2 = (current_page_ == 3) ? "Upload"   : "Write";

            float l2w = measure_text(canvas, leg2, fsz_small);
            rx -= (l2w + 12.0f * u);
            canvas.draw_text(rx, bot_y, leg2, nisaba::Paint(sec_col), fsz_small, "sans");

            float l1w = measure_text(canvas, leg1, fsz_small);
            rx -= (l1w + 12.0f * u);
            canvas.draw_text(rx, bot_y, leg1, nisaba::Paint(pri_col), fsz_small, "sans");
        }

        cy += chart_card_h + spacing;
    };

    if (current_page_ >= 1 && current_page_ <= 4) {
        std::string value;
        std::string subtitle;
        std::vector<double> primary;
        std::vector<double> secondary;
        static constexpr const char* kChartTitles[5] = {
            "", "Processor activity", "Memory usage", "Network activity", "Disk activity"
        };
        static const nisaba::Color kColors[5] = {
            accent::blue, accent::blue, accent::purple, accent::green, accent::orange
        };
        const nisaba::Color sec_col = (current_page_ == 3) ? accent::blue : accent::purple;
        const double fixed_max = (current_page_ <= 2) ? 100.0 : 0.0;

        switch (current_page_) {
            case 1:
                value = Monitor::formatPercent(metrics.cpuPercent);
                subtitle = metrics.cpuName;
                primary = history.cpuPercent;
                break;
            case 2:
                value = Monitor::formatPercent(metrics.memory.percent);
                subtitle = Monitor::formatBytes(static_cast<double>(metrics.memory.used)) + " of " +
                           Monitor::formatBytes(static_cast<double>(metrics.memory.total)) + " in use";
                primary = history.memoryPercent;
                break;
            case 3:
                value = Monitor::formatRate(metrics.download);
                subtitle = "Physical network interfaces";
                primary = history.download;
                secondary = history.upload;
                break;
            default:
                value = Monitor::formatRate(metrics.diskRead);
                subtitle = "Physical disks";
                primary = history.diskRead;
                secondary = history.diskWrite;
                break;
        }
        draw_chart_card(kChartTitles[current_page_], value, subtitle, primary, secondary,
                        kColors[current_page_], sec_col, fixed_max, false);
    }

    const float fsz_row = AppTheme::points(9.0f) * u; // 12px

    // Helper for ListCard & SectionLabel (src/views/components.cpp):
    // SectionLabel: disabledText, inset 16px, spacing 8px above ListCard
    // ListCard: Plate(theme.bigRadius = 12), InfoRow height = 44px, inset = 16px, separator = ColorId::Separator
    auto draw_list_card = [&](const std::string& section_title,
                              const std::vector<std::pair<std::string, std::string>>& rows,
                              std::optional<double> usage_bar = std::nullopt) {
        if (!section_title.empty()) {
            std::string st_elided = elide_middle(canvas, section_title, avail_w - 32.0f * u, fsz_row);
            canvas.draw_text(left_x + 16.0f * u, cy + 12.0f * u, st_elided,
                             nisaba::Paint(theme_.disabledText), fsz_row, "sans");
            cy += 22.0f * u; // label height + 8px spacing
        }

        const float row_h = 44.0f * u;
        const float bar_row_h = usage_bar.has_value() ? (22.0f * u) : 0.0f;
        const float card_h = bar_row_h + static_cast<float>(rows.size()) * row_h;
        auto card_r = *nisaba::Rect::from_xywh(left_x, cy, avail_w, card_h);
        canvas.fill_round_rect(card_r, card_rad, card_rad, nisaba::Paint(theme_.secondBackground));

        float ry = cy;
        if (usage_bar.has_value()) {
            // Storage usage ProgressBar: sits 16px below card's top, inset 16px, height 6px
            // Track: theme.alternateBackground, Fill: theme.highlight
            float prog = static_cast<float>(std::clamp(*usage_bar, 0.0, 1.0));
            float bx = left_x + 16.0f * u;
            float by = ry + 16.0f * u;
            float bw = avail_w - 32.0f * u;
            float bh = 6.0f * u;
            canvas.fill_round_rect(*nisaba::Rect::from_xywh(bx, by, bw, bh), bh * 0.5f, bh * 0.5f,
                                   nisaba::Paint(theme_.alternateBackground));
            if (prog > 0.0f) {
                float fw = std::max(bh, bw * prog);
                canvas.fill_round_rect(*nisaba::Rect::from_xywh(bx, by, fw, bh), bh * 0.5f, bh * 0.5f,
                                       nisaba::Paint(theme_.highlight));
            }
            ry += bar_row_h;
        }

        for (size_t i = 0; i < rows.size(); ++i) {
            canvas.draw_text(left_x + 16.0f * u, ry + 26.5f * u, rows[i].first,
                             nisaba::Paint(theme_.text), fsz_row, "sans");

            std::string val_elided = elide_right(canvas, rows[i].second, avail_w * 0.65f, fsz_row);
            float vw = measure_text(canvas, val_elided, fsz_row);
            canvas.draw_text(card_r.right() - 16.0f * u - vw, ry + 26.5f * u, val_elided,
                             nisaba::Paint(theme_.disabledText), fsz_row, "sans");

            if (i + 1 < rows.size()) {
                canvas.stroke_line(left_x + 16.0f * u, ry + row_h, card_r.right() - 16.0f * u, ry + row_h,
                                   nisaba::Paint(theme_.separator), nisaba::Stroke{1.0f});
            }
            ry += row_h;
        }

        cy += card_h + spacing;
    };

    // Helper for ResourcePage::addHeading (src/views/resourcepage.cpp:82-89):
    // 16px left margin, 9pt (12px), weight 500, theme.text
    auto draw_heading = [&](const char* text) {
        canvas.draw_text(left_x + 16.0f * u, cy + 12.0f * u, text,
                         nisaba::Paint(theme_.text), fsz_row, "sans");
        cy += 16.0f * u + spacing;
    };

    // ── 2. Page-Specific Sections ──────────────────────────────────────────
    switch (current_page_) {
        case 1: {
            // Logical processors grid (Tile: Plate(theme.smallRadius = 8), height 66, 8pt labels, 6px ProgressBar)
            draw_heading("Logical processors");

            int cols = std::max(2, static_cast<int>(std::floor((avail_w + 12.0f * u) / (162.0f * u))));
            float gap = 12.0f * u;
            float tile_w = (avail_w - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
            float tile_h = 66.0f * u;
            float tile_rad = theme_.smallRadius * u; // 8px
            float tile_fsz = AppTheme::points(8.0f) * u; // 10.67px

            for (size_t i = 0; i < metrics.cores.size(); ++i) {
                int r_idx = static_cast<int>(i) / cols;
                int c_idx = static_cast<int>(i) % cols;
                float tx = left_x + static_cast<float>(c_idx) * (tile_w + gap);
                float ty = cy + static_cast<float>(r_idx) * (tile_h + gap);

                auto tr = *nisaba::Rect::from_xywh(tx, ty, tile_w, tile_h);
                canvas.fill_round_rect(tr, tile_rad, tile_rad, nisaba::Paint(theme_.secondBackground));

                std::string cname = "CPU " + std::to_string(metrics.cores[i].number + 1);
                std::string cval = Monitor::formatPercent(metrics.cores[i].usage);

                canvas.draw_text(tx + 12.0f * u, ty + 22.0f * u, cname,
                                 nisaba::Paint(theme_.text), tile_fsz, "sans");
                float vw = measure_text(canvas, cval, tile_fsz);
                canvas.draw_text(tr.right() - 12.0f * u - vw, ty + 22.0f * u, cval,
                                 nisaba::Paint(theme_.text), tile_fsz, "sans");

                // ProgressBar at {12, height - 12 - 6, width - 24, 6}:
                // Track: theme.alternateBackground, Fill: theme.highlight
                float bx = tx + 12.0f * u;
                float by = tr.bottom() - 18.0f * u;
                float bw = tile_w - 24.0f * u;
                float bh = 6.0f * u;
                canvas.fill_round_rect(*nisaba::Rect::from_xywh(bx, by, bw, bh), 3.0f * u, 3.0f * u,
                                       nisaba::Paint(theme_.alternateBackground));
                float prog = static_cast<float>(std::clamp(metrics.cores[i].usage / 100.0, 0.0, 1.0));
                if (prog > 0.0f) {
                    canvas.fill_round_rect(*nisaba::Rect::from_xywh(bx, by, std::max(bh, bw * prog), bh), 3.0f * u, 3.0f * u,
                                           nisaba::Paint(theme_.highlight));
                }
            }

            int total_rows = (static_cast<int>(metrics.cores.size()) + cols - 1) / cols;
            cy += static_cast<float>(total_rows) * (tile_h + gap) + 12.0f * u;
            break;
        }
        case 2: {
            std::string swap_val = (metrics.memory.swapTotal > 0)
                ? (Monitor::formatBytes(static_cast<double>(metrics.memory.swapUsed)) + " / " +
                   Monitor::formatBytes(static_cast<double>(metrics.memory.swapTotal)))
                : "Not configured";
            draw_list_card("", {
                {"In use",       Monitor::formatBytes(static_cast<double>(metrics.memory.used))},
                {"Available",    Monitor::formatBytes(static_cast<double>(metrics.memory.available))},
                {"Cached",       Monitor::formatBytes(static_cast<double>(metrics.memory.cache))},
                {"Total memory", Monitor::formatBytes(static_cast<double>(metrics.memory.total))},
                {"Swap",         swap_val},
            });
            break;
        }
        case 3: {
            draw_list_card("", {
                {"Download",       Monitor::formatRate(metrics.download)},
                {"Upload",         Monitor::formatRate(metrics.upload)},
                {"Total received", Monitor::formatBytes(static_cast<double>(metrics.received))},
                {"Total sent",     Monitor::formatBytes(static_cast<double>(metrics.sent))},
            });

            draw_heading("Network interfaces");
            for (const auto& iface : metrics.interfaces) {
                std::string st = (iface.state == "up") ? "Connected"
                               : (iface.state == "down") ? "Disconnected" : "Unknown";
                draw_list_card(iface.name, {
                    {"State",            st},
                    {"Download",         Monitor::formatRate(iface.download)},
                    {"Upload",           Monitor::formatRate(iface.upload)},
                    {"Hardware address", iface.address.empty() ? "—" : iface.address},
                });
            }
            break;
        }
        case 4: {
            for (const auto& d : metrics.disks) {
                draw_list_card(d.model.empty() ? d.name : d.model, {
                    {"Device",   "/dev/" + d.name},
                    {"Capacity", Monitor::formatBytes(static_cast<double>(d.size))},
                    {"Read",     Monitor::formatRate(d.read)},
                    {"Write",    Monitor::formatRate(d.write)},
                });
            }

            draw_heading("Storage");
            for (const auto& vol : metrics.volumes) {
                double ratio = vol.total > 0 ? static_cast<double>(vol.used) / static_cast<double>(vol.total) : 0.0;
                std::string used_str = Monitor::formatBytes(static_cast<double>(vol.used)) + " of " +
                                       Monitor::formatBytes(static_cast<double>(vol.total));
                draw_list_card(vol.mount, {
                    {"Used",        used_str},
                    {"Available",   Monitor::formatBytes(static_cast<double>(vol.available))},
                    {"File system", vol.type.empty() ? "—" : vol.type},
                }, ratio);
            }
            break;
        }
        case 5: {
            for (size_t gi = 0; gi < metrics.gpus.size(); ++gi) {
                const auto& gpu = metrics.gpus[gi];
                const bool temp_mode = !gpu.hasBusy;
                static const std::vector<double> kNoSeries;
                const GpuHistory* gh = gi < history.gpus.size() ? &history.gpus[gi] : nullptr;
                const std::vector<double>& series = gh ? (temp_mode ? gh->temperature : gh->busy) : kNoSeries;
                draw_chart_card(temp_mode ? "Graphics temperature" : "Graphics activity",
                                temp_mode ? ((gpu.active && gpu.temperature >= 0.0) ? Monitor::formatTemperature(gpu.temperature) : "—")
                                          : Monitor::formatPercent(gpu.busy),
                                gpu.name, series, kNoSeries, accent::pink, accent::purple, 100.0, temp_mode);
                std::vector<std::pair<std::string, std::string>> rows;
                rows.push_back({"Driver", gpu.driver.empty() ? "—" : gpu.driver});
                rows.push_back({"State",  gpu.active ? "Active" : "Powered off"});
                if (gpu.hasBusy) {
                    rows.push_back({"Activity", Monitor::formatPercent(gpu.busy)});
                }
                if (gpu.hasTemperature) {
                    rows.push_back({"Temperature", (gpu.active && gpu.temperature >= 0.0)
                                                    ? Monitor::formatTemperature(gpu.temperature) : "—"});
                }
                if (gpu.hasFrequency) {
                    rows.push_back({"Clock speed", gpu.active
                                                    ? (std::to_string(std::lround(gpu.frequency)) + " MHz") : "—"});
                }
                if (gpu.hasVram) {
                    rows.push_back({"Video memory", gpu.active
                                                     ? (Monitor::formatBytes(static_cast<double>(gpu.vramUsed)) + " / " +
                                                        Monitor::formatBytes(static_cast<double>(gpu.vramTotal)))
                                                     : "—"});
                }
                draw_list_card("", rows);
            }
            break;
        }
        default:
            break;
    }

    float total_h = cy - start_y;
    res_max_scroll_y_ = std::max(0.0f, total_h - page_rect.height() + 24.0f * u);
    res_target_scroll_y_ = std::clamp(res_target_scroll_y_, 0.0f, res_max_scroll_y_);
    res_scroll_y_ = std::clamp(res_scroll_y_, 0.0f, res_max_scroll_y_);

    if (res_max_scroll_y_ > 0.0f && res_scrollbar_opacity_ > 0.01f) {
        float track_h = page_rect.height() - 16.0f * u;
        float thumb_h = std::min(track_h, std::max(32.0f * u, track_h * (page_rect.height() / total_h)));
        float thumb_y = page_rect.top() + 8.0f * u + (track_h - thumb_h) * (res_scroll_y_ / res_max_scroll_y_);
        auto sbar = *nisaba::Rect::from_xywh(page_rect.right() - 7.0f * u, thumb_y, 4.0f * u, thumb_h);
        auto tu = theme_.scrollBarThumb.to_color_u8();
        uint8_t sa = static_cast<uint8_t>(static_cast<float>(tu.a) * res_scrollbar_opacity_);
        canvas.fill_round_rect(sbar, 2.0f * u, 2.0f * u,
                               nisaba::Paint(nisaba::Color::from_rgba8(tu.r, tu.g, tu.b, sa)));
    }
}

void SystemMonitorApp::render_context_menu(nisaba::gpu::GpuCanvas& canvas) {
    const float u = scale_.u;
    // Exact FishUI Menu metrics (src/ui/menu.cpp):
    // kRowHeight = 26, kSeparatorHeight = 12, kMinimumRowWidth = 158, kContentMargin = 5, kVerticalContentMargin = 5
    const float menu_w = 168.0f * u;
    const float item_h = 26.0f * u;
    const float sep_h = 12.0f * u;
    const float pad_x = 5.0f * u;
    const float pad_y = 5.0f * u;
    const float menu_h = pad_y * 2.0f + item_h * 4.0f + sep_h;

    float mx = std::clamp(ctx_menu_x_, 8.0f * u, width_ - menu_w - 8.0f * u);
    float my = std::clamp(ctx_menu_y_, 8.0f * u, height_ - menu_h - 8.0f * u);
    rect_ctx_menu_ = *nisaba::Rect::from_xywh(mx, my, menu_w, menu_h);

    auto shadow = *nisaba::Rect::from_xywh(mx - 2.0f * u, my + 4.0f * u, menu_w + 4.0f * u, menu_h + 6.0f * u);
    canvas.fill_round_rect(shadow, (theme_.mediumRadius + 2.0f) * u, (theme_.mediumRadius + 2.0f) * u,
                           nisaba::Paint(nisaba::Color::from_rgba8(0, 0, 0, 45)));

    canvas.fill_round_rect(rect_ctx_menu_, theme_.mediumRadius * u, theme_.mediumRadius * u,
                           nisaba::Paint(theme_.secondBackground));
    canvas.stroke_round_rect(rect_ctx_menu_, theme_.mediumRadius * u, theme_.mediumRadius * u,
                             nisaba::Paint(theme_.menuSeparator), nisaba::Stroke{1.0f});

    const ProcessRecord* proc = selected_record();
    const bool has_proc = (proc != nullptr);
    const bool can_manage = (proc && Monitor::canManage(*proc));

    static constexpr const char* kItems[4] = {
        "Details",
        "Copy Process ID",
        "Quit",
        "Force Quit"
    };
    const bool enabled[4] = {has_proc, has_proc, can_manage, can_manage};
    const float fsz = AppTheme::points(9.0f) * u;

    float iy = my + pad_y;
    for (int i = 0; i < 4; ++i) {
        if (i == 2) {
            canvas.stroke_line(mx + 12.0f * u, iy + sep_h * 0.5f, mx + menu_w - 12.0f * u, iy + sep_h * 0.5f,
                               nisaba::Paint(theme_.menuSeparator), nisaba::Stroke{1.0f});
            iy += sep_h;
        }
        auto ir = *nisaba::Rect::from_xywh(mx + pad_x, iy, menu_w - pad_x * 2.0f, item_h);
        rect_ctx_items_[i] = ir;

        bool hov = (ctx_menu_hovered_ == i && enabled[i]);
        if (hov) {
            canvas.fill_round_rect(ir, 6.0f * u, 6.0f * u, nisaba::Paint(theme_.menuHighlight));
        }

        nisaba::Color fg = !enabled[i]
            ? theme_.menuDisabledText
            : (hov ? theme_.highlightedText : theme_.text);
        canvas.draw_text(ir.left() + 12.0f * u, ir.top() + 17.5f * u, kItems[i],
                         nisaba::Paint(fg), fsz, "sans");

        iy += item_h;
    }
}

void SystemMonitorApp::render_details_modal(nisaba::gpu::GpuCanvas& canvas) {
    const float u = scale_.u;

    // Subtle modal backdrop scrim
    canvas.fill_rect(*nisaba::Rect::from_xywh(0.0f, 0.0f, width_, height_),
                     nisaba::Paint(nisaba::Color::from_rgba8(0, 0, 0, theme_.darkMode ? 110 : 55)));

    const ProcessRecord* proc = find_record(details_target_);
    const bool is_app = details_target_.application;

    // Exact FishUI DialogWindow & ProcessDetails metrics (src/ui/dialog_window.cpp & src/views/processdetails.cpp):
    // Dialog width = 480, kDialogCloseInset = 10, kButton = 22, kDialogHeaderHeight = 42
    // kContentPadding = 16, kTop = 8, kIcon = 40, kSpacing = 12, kRowHeight = 36
    const float modal_w = std::min(480.0f * u, width_ - 32.0f * u);
    const float hdr_h = 42.0f * u;
    const float content_pad = 16.0f * u;
    const float row_h = 36.0f * u;

    std::vector<ProcessRecord> members = (proc && is_app) ? proc->members : std::vector<ProcessRecord>{};
    if (is_app) {
        std::sort(members.begin(), members.end(), [](const ProcessRecord& a, const ProcessRecord& b) {
            return a.memory != b.memory ? a.memory > b.memory : a.pid < b.pid;
        });
    }

    float members_card_h = 0.0f;
    if (is_app) {
        size_t vis_rows = std::clamp<size_t>(members.size(), 1, 7);
        members_card_h = 28.0f * u + static_cast<float>(vis_rows) * row_h;
    }

    float activity_card_h = (is_app ? 6.0f : 7.0f) * row_h;
    float process_card_h = is_app ? 0.0f : (5.0f * row_h + 68.0f * u);
    float total_content_h = 8.0f * u + 40.0f * u + 12.0f * u + activity_card_h + 12.0f * u +
                            (is_app ? members_card_h : process_card_h);
    float modal_h = std::min(hdr_h + total_content_h + content_pad, height_ - 24.0f * u);

    float mx = (width_ - modal_w) * 0.5f;
    float my = (height_ - modal_h) * 0.5f;
    rect_details_modal_ = *nisaba::Rect::from_xywh(mx, my, modal_w, modal_h);

    auto shadow = *nisaba::Rect::from_xywh(mx - 4.0f * u, my + 6.0f * u, modal_w + 8.0f * u, modal_h + 10.0f * u);
    canvas.fill_round_rect(shadow, (theme_.windowRadius + 4.0f) * u, (theme_.windowRadius + 4.0f) * u,
                           nisaba::Paint(nisaba::Color::from_rgba8(0, 0, 0, 55)));

    canvas.fill_round_rect(rect_details_modal_, theme_.windowRadius * u, theme_.windowRadius * u,
                           nisaba::Paint(theme_.background));
    canvas.stroke_round_rect(rect_details_modal_, theme_.windowRadius * u, theme_.windowRadius * u,
                             nisaba::Paint(theme_.separator), nisaba::Stroke{1.0f});

    // Dialog Header: Lone close button at top-left (inset 10, size 22x22, artwork "dialog-close")
    // Title centered on the window at 12px
    rect_details_close_ = *nisaba::Rect::from_xywh(mx + 10.0f * u, my + 10.0f * u, 22.0f * u, 22.0f * u);
    CsdFrame::paint_window_control(canvas, rect_details_close_, "dialog-close",
                                   theme_.darkMode, true, true,
                                   details_close_hovered_, details_close_pressed_, u);

    std::string dlg_title = is_app ? "Application Details" : "Process Details";
    float dtw = measure_text(canvas, dlg_title, 12.0f * u);
    canvas.draw_text(mx + (modal_w - dtw) * 0.5f, my + 25.0f * u, dlg_title,
                     nisaba::Paint(theme_.text), 12.0f * u, "sans");

    float cx = mx + content_pad;
    float cw = modal_w - 2.0f * content_pad;
    float cy = my + hdr_h + 8.0f * u;

    // Header Row: 40x40 Icon + Name (12pt = 16px, weight 500) + Subtitle (9pt = 12px, disabledText)
    std::string disp_name = proc ? proc->displayName : (is_app ? "Application quit" : "Process exited");
    std::string icon_name = (proc && !proc->iconName.empty()) ? proc->iconName : "application-x-executable";

    VectorIcons::draw_process_badge(canvas, cx + 20.0f * u, cy + 20.0f * u, 40.0f * u,
                                    icon_name, disp_name, theme_, false);

    float fsz_name = AppTheme::points(12.0f) * u; // 16px
    float fsz_row  = AppTheme::points(9.0f) * u;  // 12px
    float fsz_hdr  = AppTheme::points(8.0f) * u;  // 10.67px

    canvas.draw_text(cx + 52.0f * u, cy + 17.0f * u,
                     elide_right(canvas, disp_name, cw - 52.0f * u, fsz_name),
                     nisaba::Paint(theme_.text), fsz_name, "sans");

    std::string sub = "PID " + std::to_string(details_target_.pid);
    if (proc && is_app) {
        sub += " · " + std::to_string(proc->members.size()) + (proc->members.size() == 1 ? " process" : " processes");
    } else if (proc) {
        std::string st = Monitor::describeProcessState(proc->state);
        if (!st.empty()) sub += " · " + st;
        if (!proc->user.empty()) sub += " · " + proc->user;
    }
    canvas.draw_text(cx + 52.0f * u, cy + 35.0f * u, sub,
                     nisaba::Paint(theme_.disabledText), fsz_row, "sans");
    cy += 40.0f * u + 12.0f * u;

    // Compute IO totals
    uint64_t read_bytes = proc ? proc->readBytes : 0;
    uint64_t write_bytes = proc ? proc->writeBytes : 0;
    if (proc && is_app) {
        read_bytes = 0;
        write_bytes = 0;
        for (const auto& m : proc->members) {
            if (m.ioAvailable) {
                read_bytes += m.readBytes;
                write_bytes += m.writeBytes;
            }
        }
    }
    const bool io_ok = (proc && proc->ioAvailable);
    std::string read_str = io_ok
        ? (Monitor::formatRate(proc->readRate) + ", " + Monitor::formatBytes(static_cast<double>(read_bytes)) + " in total")
        : "—";
    std::string write_str = io_ok
        ? (Monitor::formatRate(proc->writeRate) + ", " + Monitor::formatBytes(static_cast<double>(write_bytes)) + " in total")
        : "—";

    const float card_rad = theme_.bigRadius * u; // 12px

    auto draw_modal_list_card = [&](const std::vector<std::pair<std::string, std::string>>& rows,
                                    const std::string& command_line = "") {
        float cmd_h = command_line.empty() ? 0.0f : (68.0f * u);
        float ch = static_cast<float>(rows.size()) * row_h + cmd_h;
        auto cr = *nisaba::Rect::from_xywh(cx, cy, cw, ch);
        canvas.fill_round_rect(cr, card_rad, card_rad, nisaba::Paint(theme_.secondBackground));

        float ry = cy;
        for (size_t i = 0; i < rows.size(); ++i) {
            canvas.draw_text(cx + 16.0f * u, ry + 22.5f * u, rows[i].first,
                             nisaba::Paint(theme_.text), fsz_row, "sans");
            std::string ve = elide_middle(canvas, rows[i].second, cw * 0.65f, fsz_row);
            float vw = measure_text(canvas, ve, fsz_row);
            canvas.draw_text(cr.right() - 16.0f * u - vw, ry + 22.5f * u, ve,
                             nisaba::Paint(theme_.disabledText), fsz_row, "sans");
            if (i + 1 < rows.size() || !command_line.empty()) {
                canvas.stroke_line(cx + 16.0f * u, ry + row_h, cr.right() - 16.0f * u, ry + row_h,
                                   nisaba::Paint(theme_.separator), nisaba::Stroke{1.0f});
            }
            ry += row_h;
        }

        if (!command_line.empty()) {
            // CommandRow: title "Command" at top, followed by up to 2 lines of command text in disabledText
            canvas.draw_text(cx + 16.0f * u, ry + 22.0f * u, "Command",
                             nisaba::Paint(theme_.text), fsz_row, "sans");
            std::string l1 = elide_right(canvas, command_line, cw - 32.0f * u, fsz_row);
            canvas.draw_text(cx + 16.0f * u, ry + 42.0f * u, l1,
                             nisaba::Paint(theme_.disabledText), fsz_row, "sans");
        }

        cy += ch + 12.0f * u;
    };

    if (is_app) {
        draw_modal_list_card({
            {"CPU",          proc ? Monitor::formatPercent(proc->cpu) : "—"},
            {"Memory",       proc ? Monitor::formatBytes(static_cast<double>(proc->memory)) : "—"},
            {"Disk read",    read_str},
            {"Disk write",   write_str},
            {"Threads",      (proc && proc->threads > 0) ? std::to_string(proc->threads) : "—"},
            {"Running time", proc ? format_running_time(*proc) : "—"},
        });

        // MembersCard (Plate(theme.bigRadius), header height 28, row height 36)
        auto mcr = *nisaba::Rect::from_xywh(cx, cy, cw, members_card_h);
        canvas.fill_round_rect(mcr, card_rad, card_rad, nisaba::Paint(theme_.secondBackground));

        const float mhdr_h = 28.0f * u;
        canvas.draw_text(cx + 16.0f * u, cy + 18.5f * u, "Name",
                         nisaba::Paint(theme_.disabledText), fsz_hdr, "sans");

        const float pid_col_w = 56.0f * u;
        const float mem_col_w = 80.0f * u;
        const float cpu_col_w = 64.0f * u;
        float rx = mcr.right() - 16.0f * u;

        canvas.draw_text(rx - measure_text(canvas, "PID", fsz_hdr), cy + 18.5f * u, "PID",
                         nisaba::Paint(theme_.disabledText), fsz_hdr, "sans");
        rx -= pid_col_w;

        canvas.draw_text(rx - measure_text(canvas, "Memory", fsz_hdr), cy + 18.5f * u, "Memory",
                         nisaba::Paint(theme_.disabledText), fsz_hdr, "sans");
        rx -= mem_col_w;

        canvas.draw_text(rx - measure_text(canvas, "CPU", fsz_hdr), cy + 18.5f * u, "CPU",
                         nisaba::Paint(theme_.disabledText), fsz_hdr, "sans");

        canvas.stroke_line(cx + 16.0f * u, cy + mhdr_h, mcr.right() - 16.0f * u, cy + mhdr_h,
                           nisaba::Paint(theme_.separator), nisaba::Stroke{1.0f});

        rect_details_members_area_ = *nisaba::Rect::from_xywh(cx, cy + mhdr_h, cw, std::max(0.0f, members_card_h - mhdr_h));
        details_members_max_scroll_y_ = std::max(0.0f, static_cast<float>(members.size()) * row_h - rect_details_members_area_.height());
        details_members_scroll_y_ = std::clamp(details_members_scroll_y_, 0.0f, details_members_max_scroll_y_);

        canvas.save();
        canvas.clip_rrect(mcr, nisaba::BorderRadius::all(card_rad));
        canvas.clip_rect(rect_details_members_area_);

        float mry = rect_details_members_area_.top() - details_members_scroll_y_;
        for (size_t i = 0; i < members.size(); ++i) {
            const auto& m = members[i];
            if (mry + row_h >= rect_details_members_area_.top() && mry <= rect_details_members_area_.bottom()) {
                std::string pname = elide_right(canvas, ProcessList::programName(m),
                                                cw - pid_col_w - mem_col_w - cpu_col_w - 44.0f * u, fsz_row);
                canvas.draw_text(cx + 16.0f * u, mry + 22.5f * u, pname,
                                 nisaba::Paint(theme_.text), fsz_row, "sans");

                float mrx = mcr.right() - 16.0f * u;
                std::string p_str = std::to_string(m.pid);
                canvas.draw_text(mrx - measure_text(canvas, p_str, fsz_row), mry + 22.5f * u, p_str,
                                 nisaba::Paint(theme_.disabledText), fsz_row, "sans");
                mrx -= pid_col_w;

                std::string mem_s = Monitor::formatBytes(static_cast<double>(m.memory));
                canvas.draw_text(mrx - measure_text(canvas, mem_s, fsz_row), mry + 22.5f * u, mem_s,
                                 nisaba::Paint(theme_.text), fsz_row, "sans");
                mrx -= mem_col_w;

                std::string cpu_s = Monitor::formatPercent(m.cpu);
                canvas.draw_text(mrx - measure_text(canvas, cpu_s, fsz_row), mry + 22.5f * u, cpu_s,
                                 nisaba::Paint(theme_.text), fsz_row, "sans");

                if (i + 1 < members.size()) {
                    canvas.stroke_line(cx + 16.0f * u, mry + row_h, mcr.right() - 16.0f * u, mry + row_h,
                                       nisaba::Paint(theme_.separator), nisaba::Stroke{1.0f});
                }
            }
            mry += row_h;
        }
        canvas.restore();
    } else {
        ProcessExtras extras = proc
            ? Monitor::inspectProcessExtras(proc->pid, proc->startTicks)
            : ProcessExtras{};

        draw_modal_list_card({
            {"CPU",             proc ? Monitor::formatPercent(proc->cpu) : "—"},
            {"Memory",          proc ? Monitor::formatBytes(static_cast<double>(proc->memory)) : "—"},
            {"Resident memory", proc ? Monitor::formatBytes(static_cast<double>(proc->rss)) : "—"},
            {"Disk read",       read_str},
            {"Disk write",      write_str},
            {"Threads",         (proc && proc->threads > 0) ? std::to_string(proc->threads) : "—"},
            {"Open files",      extras.openFiles >= 0 ? std::to_string(extras.openFiles) : "—"},
        });

        std::string parent_str = "—";
        if (proc && proc->parentPid > 0) {
            const ProcessRecord* par = monitor_.processes().find(proc->parentPid);
            parent_str = par
                ? (par->displayName + " (" + std::to_string(proc->parentPid) + ")")
                : std::to_string(proc->parentPid);
        }

        std::string cmd_str = (proc && !proc->command.empty())
            ? proc->command
            : ((proc && !proc->executable.empty()) ? proc->executable : "—");

        draw_modal_list_card({
            {"Parent process", parent_str},
            {"Running time",   proc ? format_running_time(*proc) : "—"},
            {"Nice value",     proc ? std::to_string(proc->nice) : "—"},
            {"Unit",           !extras.unit.empty() ? extras.unit : "—"},
            {"Executable",     (proc && !proc->executable.empty()) ? proc->executable : "—"},
        }, cmd_str);
    }
}

void SystemMonitorApp::render_confirm_modal(nisaba::gpu::GpuCanvas& canvas) {
    const float u = scale_.u;

    // Exact FishUI ConfirmDialog / AlertView specification (src/ui/dialog_window.cpp:384-621):
    // AlertBackground = theme.alertBackground (#FFFFFF in light, #343434 in dark)
    // kAlertInset = 16, kAlertTextInset = 22, kAlertTop = 19, kAlertTextSize = 13,
    // kAlertButtonHeight = 28, kAlertButtonSpacing = 8, pill radius = 14 (height / 2)
    // Destructive button: DialogButtonDestructiveBackground (25% translucent red) + DialogButtonDestructiveText (red text)
    canvas.fill_rect(*nisaba::Rect::from_xywh(0.0f, 0.0f, width_, height_),
                     nisaba::Paint(nisaba::Color::from_rgba8(0, 0, 0, theme_.darkMode ? 110 : 55)));

    const ProcessRecord* proc = find_record(confirm_target_);
    std::string name = proc ? proc->displayName : ("PID " + std::to_string(confirm_target_.pid));

    std::string title = (confirm_force_ ? "Force quit " : "Quit ") + name + "?";
    std::string body = confirm_target_.application
        ? (confirm_force_ ? "All of its processes will stop immediately. Unsaved work may be lost."
                          : "The application will be asked to quit. Unsaved work may be lost.")
        : (confirm_force_ ? "The process will stop immediately. Unsaved work may be lost."
                          : "The process will be asked to quit. Unsaved work may be lost.");

    float mw = 340.0f * u;
    float mh = 138.0f * u;
    float mx = (width_ - mw) * 0.5f;
    float my = (height_ - mh) * 0.5f;
    rect_confirm_modal_ = *nisaba::Rect::from_xywh(mx, my, mw, mh);

    auto shadow = *nisaba::Rect::from_xywh(mx - 4.0f * u, my + 6.0f * u, mw + 8.0f * u, mh + 10.0f * u);
    canvas.fill_round_rect(shadow, (theme_.windowRadius + 4.0f) * u, (theme_.windowRadius + 4.0f) * u,
                           nisaba::Paint(nisaba::Color::from_rgba8(0, 0, 0, 55)));

    canvas.fill_round_rect(rect_confirm_modal_, theme_.windowRadius * u, theme_.windowRadius * u,
                           nisaba::Paint(theme_.alertBackground));
    canvas.stroke_round_rect(rect_confirm_modal_, theme_.windowRadius * u, theme_.windowRadius * u,
                             nisaba::Paint(theme_.separator), nisaba::Stroke{1.0f});

    float text_x = mx + 22.0f * u;
    float text_w = mw - 44.0f * u;
    float fsz = 13.0f * u;

    canvas.draw_text(text_x, my + 32.0f * u, elide_right(canvas, title, text_w, fsz),
                     nisaba::Paint(theme_.alertText), fsz, "sans");

    // Wrap body across two lines if needed
    std::string line1 = body;
    std::string line2;
    if (measure_text(canvas, body, 12.0f * u) > text_w) {
        size_t split_pos = body.rfind(' ', body.size() / 2 + 10);
        if (split_pos != std::string::npos) {
            line1 = body.substr(0, split_pos);
            line2 = body.substr(split_pos + 1);
        }
    }
    canvas.draw_text(text_x, my + 54.0f * u, line1, nisaba::Paint(theme_.alertText), 12.0f * u, "sans");
    if (!line2.empty()) {
        canvas.draw_text(text_x, my + 70.0f * u, line2, nisaba::Paint(theme_.alertText), 12.0f * u, "sans");
    }

    float inset = 16.0f * u;
    float btn_spacing = 8.0f * u;
    float btn_w = (mw - 2.0f * inset - btn_spacing) * 0.5f;
    float btn_h = 28.0f * u;
    float btn_y = my + mh - inset - btn_h;
    float btn_rad = btn_h * 0.5f;

    rect_confirm_cancel_ = *nisaba::Rect::from_xywh(mx + inset, btn_y, btn_w, btn_h);
    rect_confirm_accept_ = *nisaba::Rect::from_xywh(rect_confirm_cancel_.right() + btn_spacing, btn_y, btn_w, btn_h);

    // Cancel Button (DialogButtonBackground / DialogButtonBackgroundPressed, text = AlertText)
    nisaba::Color cancel_bg = (confirm_pressed_btn_ == 0)
        ? theme_.dialogButtonBackgroundPressed
        : theme_.dialogButtonBackground;
    canvas.fill_round_rect(rect_confirm_cancel_, btn_rad, btn_rad, nisaba::Paint(cancel_bg));
    float cw = measure_text(canvas, "Cancel", fsz);
    canvas.draw_text(rect_confirm_cancel_.left() + (btn_w - cw) * 0.5f, btn_y + 18.5f * u, "Cancel",
                     nisaba::Paint(theme_.alertText), fsz, "sans");

    // Destructive Confirm Button (DialogButtonDestructiveBackground + DialogButtonDestructiveText)
    canvas.fill_round_rect(rect_confirm_accept_, btn_rad, btn_rad, nisaba::Paint(theme_.dialogButtonDestructiveBg));
    if (confirm_pressed_btn_ == 1) {
        nisaba::Color veil = detail::with_alpha(
            theme_.darkMode ? detail::rgb_hex(0xFFFFFF) : detail::rgb_hex(0x000000),
            theme_.darkMode ? 0.035f : 0.04f
        );
        canvas.fill_round_rect(rect_confirm_accept_, btn_rad, btn_rad, nisaba::Paint(veil));
    }
    std::string acc_txt = confirm_force_ ? "Force Quit" : "Quit";
    float aw = measure_text(canvas, acc_txt, fsz);
    canvas.draw_text(rect_confirm_accept_.left() + (btn_w - aw) * 0.5f, btn_y + 18.5f * u, acc_txt,
                     nisaba::Paint(theme_.dialogButtonDestructiveText), fsz, "sans");
}

void SystemMonitorApp::render_toast(nisaba::gpu::GpuCanvas& canvas) {
    const float u = scale_.u;
    // Exact FishUI Toast Bubble (src/ui/toast.cpp:110-118):
    // radius = theme.mediumRadius (10), fill = theme.background with 0.9 opacity,
    // padding = theme.largeSpacing (12), shadow = ColorId::ToastShadow
    float alpha_f = std::clamp(toast_timer_ * 2.0f, 0.0f, 1.0f) * 0.90f;
    float fsz = AppTheme::points(9.0f) * u;
    float pad = theme_.largeSpacing * u;
    float tw = measure_text(canvas, toast_text_, fsz) + 2.0f * pad;
    float th = 38.0f * u;
    auto tr = *nisaba::Rect::from_xywh((width_ - tw) * 0.5f, height_ - th - 16.0f * u, tw, th);

    float rad = theme_.mediumRadius * u;
    auto sh = *nisaba::Rect::from_xywh(tr.left() - 2.0f * u, tr.top() + 3.0f * u, tr.width() + 4.0f * u, tr.height() + 4.0f * u);
    canvas.fill_round_rect(sh, rad + 2.0f * u, rad + 2.0f * u, nisaba::Paint(theme_.toastShadow));

    canvas.fill_round_rect(tr, rad, rad, nisaba::Paint(detail::with_alpha(theme_.background, alpha_f)));
    canvas.stroke_round_rect(tr, rad, rad, nisaba::Paint(theme_.separator), nisaba::Stroke{1.0f});
    canvas.draw_text(tr.left() + pad, tr.top() + 23.5f * u, toast_text_,
                     nisaba::Paint(detail::with_alpha(theme_.text, alpha_f)), fsz, "sans");
}

bool SystemMonitorApp::handle_mouse_down(float x, float y, int button) {
    mouse_x_ = x;
    mouse_y_ = y;

    // 1. Confirm Modal intercepts all clicks when open
    if (confirm_open_) {
        if (button == 1) {
            if (rect_contains(rect_confirm_cancel_, x, y)) {
                confirm_pressed_btn_ = 0;
                return true;
            }
            if (rect_contains(rect_confirm_accept_, x, y)) {
                confirm_pressed_btn_ = 1;
                return true;
            }
            if (!rect_contains(rect_confirm_modal_, x, y)) {
                confirm_open_ = false;
            }
        }
        return true;
    }

    // 2. Details Modal intercepts all clicks when open
    if (details_open_) {
        if (button == 1) {
            if (rect_contains(rect_details_close_, x, y)) {
                details_close_pressed_ = true;
                return true;
            }
            if (!rect_contains(rect_details_modal_, x, y)) {
                details_open_ = false;
                return true;
            }
        }
        return true;
    }

    // 3. Context Menu Overlay
    if (ctx_menu_open_) {
        if (button == 1 && rect_contains(rect_ctx_menu_, x, y)) {
            const ProcessRecord* proc = selected_record();
            const bool has_proc = (proc != nullptr);
            const bool can_manage = (proc && Monitor::canManage(*proc));

            if (rect_contains(rect_ctx_items_[0], x, y) && has_proc) {
                open_details_modal();
            } else if (rect_contains(rect_ctx_items_[1], x, y) && has_proc) {
                if (platform_) {
                    platform_->setClipboardText(std::to_string(selection_.pid));
                }
                ctx_menu_open_ = false;
            } else if (rect_contains(rect_ctx_items_[2], x, y) && can_manage) {
                open_confirm_modal(false);
            } else if (rect_contains(rect_ctx_items_[3], x, y) && can_manage) {
                open_confirm_modal(true);
            }
            return true;
        }
        ctx_menu_open_ = false;
        if (button == 1) return true;
    }

    // 4. Header Light/Dark Mode Toggle Button (check before CSD titlebar drag)
    if (button == 1 && rect_contains(rect_theme_toggle_, x, y)) {
        theme_toggle_pressed_ = true;
        return true;
    }

    // 5. CSD Window Frame, Titlebar Drag & 8-Edge Resize
    if (csd_frame_.handle_mouse_down(x, y, button)) {
        search_focused_ = false;
        return true;
    }

    // 6. Left Sidebar Navigation Cards
    if (button == 1) {
        for (size_t i = 0; i < sidebar_rects_.size(); ++i) {
            if (rect_contains(sidebar_rects_[i], x, y)) {
                set_current_page(static_cast<int>(i));
                search_focused_ = false;
                return true;
            }
        }
    }

    // 7. Processes Page Interactions
    if (current_page_ == 0) {
        if (button == 1) {
            // Search box clear & focus
            if (!monitor_.processes().search().empty() && rect_contains(rect_search_clear_, x, y)) {
                monitor_.processes().setSearch("");
                monitor_.applications().setSearch("");
                clear_selection();
                return true;
            }
            if (rect_contains(rect_search_box_, x, y)) {
                search_focused_ = true;
                return true;
            }
            search_focused_ = false;

            // Scope segmented control
            for (int i = 0; i < 3; ++i) {
                if (rect_contains(rect_scope_segs_[i], x, y)) {
                    pressed_scope_seg_ = i;
                    return true;
                }
            }

            // Quit button
            if (rect_contains(rect_quit_btn_, x, y)) {
                quit_btn_pressed_ = true;
                return true;
            }

            // Column headers sorting
            for (size_t i = 0; i < kTableCols.size(); ++i) {
                if (rect_contains(rect_col_headers_[i], x, y)) {
                    monitor_.sortBy(kTableCols[i].key);
                    return true;
                }
            }

            // Process table rows (single click selects, double click opens details)
            if (rect_contains(rect_table_body_, x, y)) {
                float row_h = 36.0f * scale_.u;
                float rel_y = (y - rect_table_body_.top()) + proc_scroll_y_;
                size_t row = static_cast<size_t>(std::max(0.0f, std::floor(rel_y / row_h)));
                if (row < active_list().count()) {
                    select_row(row);
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_row_click_time_).count();
                    if (selection_.pid > 0 && selection_.pid == last_clicked_pid_ && elapsed_ms < 400) {
                        open_details_modal();
                        last_clicked_pid_ = -1;
                    } else {
                        last_row_click_time_ = now;
                        last_clicked_pid_ = selection_.pid;
                    }
                    return true;
                }
            }
        } else if (button == 3) {
            // Right-click on a process row opens the context menu
            if (rect_contains(rect_table_body_, x, y)) {
                float row_h = 36.0f * scale_.u;
                float rel_y = (y - rect_table_body_.top()) + proc_scroll_y_;
                size_t row = static_cast<size_t>(std::max(0.0f, std::floor(rel_y / row_h)));
                if (row < active_list().count()) {
                    select_row(row);
                    ctx_menu_x_ = x;
                    ctx_menu_y_ = y;
                    ctx_menu_open_ = true;
                    return true;
                }
            }
        }
    }

    return false;
}

void SystemMonitorApp::handle_mouse_move(float x, float y) {
    mouse_x_ = x;
    mouse_y_ = y;

    if (confirm_open_) {
        confirm_hovered_btn_ = -1;
        if (rect_contains(rect_confirm_cancel_, x, y)) confirm_hovered_btn_ = 0;
        else if (rect_contains(rect_confirm_accept_, x, y)) confirm_hovered_btn_ = 1;
        return;
    }

    if (details_open_) {
        details_close_hovered_ = rect_contains(rect_details_close_, x, y);
        return;
    }

    if (ctx_menu_open_) {
        ctx_menu_hovered_ = -1;
        for (int i = 0; i < 4; ++i) {
            if (rect_contains(rect_ctx_items_[i], x, y)) {
                ctx_menu_hovered_ = i;
                break;
            }
        }
        return;
    }

    theme_toggle_hovered_ = rect_contains(rect_theme_toggle_, x, y);
    csd_frame_.handle_mouse_move(x, y, width_, height_, scale_);

    hovered_sidebar_item_ = -1;
    for (size_t i = 0; i < sidebar_rects_.size(); ++i) {
        if (rect_contains(sidebar_rects_[i], x, y)) {
            hovered_sidebar_item_ = static_cast<int>(i);
            break;
        }
    }

    hovered_scope_seg_ = -1;
    hovered_sort_col_ = -1;
    hovered_proc_row_ = -1;
    quit_btn_hovered_ = false;

    if (current_page_ == 0) {
        for (int i = 0; i < 3; ++i) {
            if (rect_contains(rect_scope_segs_[i], x, y)) {
                hovered_scope_seg_ = i;
                break;
            }
        }
        for (size_t i = 0; i < kTableCols.size(); ++i) {
            if (rect_contains(rect_col_headers_[i], x, y)) {
                hovered_sort_col_ = static_cast<int>(i);
                break;
            }
        }
        quit_btn_hovered_ = rect_contains(rect_quit_btn_, x, y);

        if (rect_contains(rect_table_body_, x, y)) {
            float row_h = 36.0f * scale_.u;
            float rel_y = (y - rect_table_body_.top()) + proc_scroll_y_;
            int row = static_cast<int>(std::floor(rel_y / row_h));
            if (row >= 0 && static_cast<size_t>(row) < active_list().count()) {
                hovered_proc_row_ = row;
            }
        }
    }
}

void SystemMonitorApp::handle_mouse_up(float x, float y, int button) {
    if (button == 1) {
        if (confirm_open_ && confirm_pressed_btn_ >= 0) {
            int act = confirm_pressed_btn_;
            confirm_pressed_btn_ = -1;
            if (act == 0 && rect_contains(rect_confirm_cancel_, x, y)) {
                confirm_open_ = false;
            } else if (act == 1 && rect_contains(rect_confirm_accept_, x, y)) {
                confirm_open_ = false;
                std::string err = confirm_target_.application
                    ? monitor_.endApplication(confirm_target_.pid, confirm_target_.startTicks, confirm_force_)
                    : monitor_.endProcess(confirm_target_.pid, confirm_target_.startTicks, confirm_force_);
                if (!err.empty()) {
                    show_toast(err);
                }
            }
            return;
        }

        if (details_open_ && details_close_pressed_) {
            details_close_pressed_ = false;
            if (rect_contains(rect_details_close_, x, y)) {
                details_open_ = false;
            }
            return;
        }

        if (theme_toggle_pressed_) {
            theme_toggle_pressed_ = false;
            if (rect_contains(rect_theme_toggle_, x, y)) {
                toggle_dark_mode();
            }
            return;
        }

        if (pressed_scope_seg_ >= 0) {
            int seg = pressed_scope_seg_;
            pressed_scope_seg_ = -1;
            if (rect_contains(rect_scope_segs_[seg], x, y) && seg != monitor_.scope()) {
                monitor_.setScope(seg);
                clear_selection();
                proc_scroll_y_ = 0.0f;
                proc_target_scroll_y_ = 0.0f;
            }
            return;
        }

        if (quit_btn_pressed_) {
            quit_btn_pressed_ = false;
            if (rect_contains(rect_quit_btn_, x, y)) {
                open_confirm_modal(false);
            }
            return;
        }
    }

    csd_frame_.handle_mouse_up(x, y, button);
}

bool SystemMonitorApp::handle_scroll(float x, float y, float dy) {
    if (confirm_open_) return true;

    if (details_open_) {
        if (details_target_.application && rect_contains(rect_details_members_area_, x, y)) {
            details_members_scroll_y_ = std::clamp(details_members_scroll_y_ - dy * 36.0f * scale_.u,
                                                   0.0f, details_members_max_scroll_y_);
        }
        return true;
    }

    if (ctx_menu_open_) {
        ctx_menu_open_ = false;
    }

    if (current_page_ == 0 && rect_contains(rect_table_, x, y)) {
        proc_target_scroll_y_ = std::clamp(proc_target_scroll_y_ - dy * 48.0f * scale_.u, 0.0f, proc_max_scroll_y_);
        proc_scrollbar_timer_ = 1.25f;
        return true;
    } else if (current_page_ > 0 && x >= 240.0f * scale_.u) {
        res_target_scroll_y_ = std::clamp(res_target_scroll_y_ - dy * 52.0f * scale_.u, 0.0f, res_max_scroll_y_);
        res_scrollbar_timer_ = 1.25f;
        return true;
    }
    return false;
}

bool SystemMonitorApp::handle_key_down(int keycode, int modifiers) {
    const bool ctrl = (modifiers & static_cast<int>(nisaba::backend_os::KeyMod::Ctrl)) != 0;

    // Ctrl+Q quits application
    if (ctrl && (keycode == 'q' || keycode == 'Q')) {
        should_quit_ = true;
        return true;
    }

    // Ctrl+D or Ctrl+T toggles Light / Dark theme mode
    if (ctrl && (keycode == 'd' || keycode == 'D' || keycode == 't' || keycode == 'T')) {
        toggle_dark_mode();
        return true;
    }

    // Ctrl+F focuses search bar on Processes page
    if (ctrl && (keycode == 'f' || keycode == 'F')) {
        confirm_open_ = false;
        details_open_ = false;
        ctx_menu_open_ = false;
        set_current_page(0);
        search_focused_ = true;
        return true;
    }

    // Escape closes modals, clears search, or returns to Processes page
    if (keycode == 27 || keycode == 65307) {
        if (confirm_open_) {
            confirm_open_ = false;
            return true;
        }
        if (details_open_) {
            details_open_ = false;
            return true;
        }
        if (ctx_menu_open_) {
            ctx_menu_open_ = false;
            return true;
        }
        if (search_focused_ || !monitor_.processes().search().empty()) {
            search_focused_ = false;
            monitor_.processes().setSearch("");
            monitor_.applications().setSearch("");
            clear_selection();
            return true;
        }
        if (current_page_ != 0) {
            set_current_page(0);
            return true;
        }
        return false;
    }

    if (confirm_open_) {
        if (keycode == 13 || keycode == 65293) {
            confirm_open_ = false;
            std::string err = confirm_target_.application
                ? monitor_.endApplication(confirm_target_.pid, confirm_target_.startTicks, confirm_force_)
                : monitor_.endProcess(confirm_target_.pid, confirm_target_.startTicks, confirm_force_);
            if (!err.empty()) show_toast(err);
            return true;
        }
        return false;
    }

    if (details_open_) return false;

    // Search input backspace handling
    if (search_focused_) {
        if (keycode == 8 || keycode == 65288) {
            std::string q = monitor_.processes().search();
            if (!q.empty()) {
                q.pop_back();
                while (!q.empty() && (static_cast<uint8_t>(q.back()) & 0xC0) == 0x80) {
                    q.pop_back();
                }
                monitor_.processes().setSearch(q);
                monitor_.applications().setSearch(q);
                clear_selection();
            }
            return true;
        }
        if (keycode == 13 || keycode == 65293) {
            search_focused_ = false;
            return true;
        }
        return false;
    }

    // Process table keyboard navigation: Up, Down, Enter (Details), Delete (Quit)
    if (current_page_ == 0) {
        ProcessList& lst = active_list();
        const int count = static_cast<int>(lst.count());
        if (count > 0) {
            if (keycode == 65362 || keycode == 103) { // Up arrow
                int cur = lst.indexOf(selection_.pid);
                int row = std::clamp(cur <= 0 ? 0 : cur - 1, 0, count - 1);
                select_row(static_cast<size_t>(row));
                scroll_to_row(static_cast<size_t>(row));
                return true;
            }
            if (keycode == 65364 || keycode == 108) { // Down arrow
                int cur = lst.indexOf(selection_.pid);
                int row = std::clamp(cur < 0 ? 0 : cur + 1, 0, count - 1);
                select_row(static_cast<size_t>(row));
                scroll_to_row(static_cast<size_t>(row));
                return true;
            }
            if (keycode == 13 || keycode == 65293) { // Return / Enter -> Details
                open_details_modal();
                return true;
            }
            if (keycode == 127 || keycode == 65535) { // Delete -> Quit
                open_confirm_modal(false);
                return true;
            }
        }
    }

    return false;
}

bool SystemMonitorApp::handle_text_input(std::string_view text) {
    if (!search_focused_ || confirm_open_ || details_open_) return false;
    std::string filtered;
    for (char c : text) {
        if (static_cast<unsigned char>(c) >= 32 && c != 127) {
            filtered += c;
        }
    }
    if (filtered.empty()) return false;
    std::string q = monitor_.processes().search() + filtered;
    monitor_.processes().setSearch(q);
    monitor_.applications().setSearch(q);
    clear_selection();
    proc_scroll_y_ = 0.0f;
    proc_target_scroll_y_ = 0.0f;
    return true;
}

} // namespace sysmon
