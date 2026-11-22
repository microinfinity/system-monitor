#pragma once
/// @file system_monitor_app.hpp
/// @brief Exact 1:1 Obsidian System Monitor Application on Nisaba GPU Engine.
/// Uses the standalone AppTheme (Light & Dark Modes) and Obsidian CSD Window Controls.

#include "nisaba/gpu/gpu_canvas.hpp"
#include "nisaba/backend_os/platform.hpp"
#include "nisaba/backend_os/window.hpp"

#include "csd_frame.hpp"
#include "theme.hpp"
#include "system_telemetry.hpp"
#include <obsidian/theme_client.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace sysmon {

struct SelectionTarget {
    int pid = 0;
    uint64_t startTicks = 0;
    bool application = false;
};

class SystemMonitorApp {
public:
    explicit SystemMonitorApp(Monitor& monitor, float width = 907.0f, float height = 600.0f, bool dark_mode = false);
    ~SystemMonitorApp();

    void set_window_and_platform(nisaba::backend_os::Window* win, nisaba::backend_os::Platform* plat);

    void update(float dt);
    void render(nisaba::gpu::GpuCanvas& canvas);
    void resize(float width, float height);

    bool handle_mouse_down(float x, float y, int button = 1);
    void handle_mouse_move(float x, float y);
    void handle_mouse_up(float x, float y, int button = 1);
    bool handle_scroll(float x, float y, float dy);
    bool handle_key_down(int keycode, int modifiers);
    bool handle_text_input(std::string_view text);

    void set_current_page(int page);
    [[nodiscard]] int current_page() const noexcept { return current_page_; }

    void set_dark_mode(bool dark);
    void toggle_dark_mode() { set_dark_mode(!dark_mode_); }
    [[nodiscard]] bool is_dark_mode() const noexcept { return dark_mode_; }

    [[nodiscard]] bool should_quit() const noexcept { return should_quit_; }

private:
    Monitor& monitor_;
    nisaba::backend_os::Window* window_{nullptr};
    nisaba::backend_os::Platform* platform_{nullptr};
    CsdFrame csd_frame_;

    float width_{907.0f};
    float height_{600.0f};
    UnitScale scale_;

    // Obsidian Theme Manager Integration (Own Light/Dark + Shell Live Monet Seed + Apps Opacity)
    bool dark_mode_{false};
    AppTheme theme_{AppTheme::light()};
    AppTheme target_theme_{AppTheme::light()};
    float window_opacity_{0.85f};
    float target_window_opacity_{0.85f};
    obsidian::ThemeClient theme_client_;
    std::mutex theme_ev_mutex_;
    obsidian::ThemeEvent pending_theme_ev_{};
    std::atomic<bool> theme_ev_dirty_{false};

    void apply_system_theme(const obsidian::ThemeEvent& ev);

    // Subtle header theme toggle button (Sun / Moon) at top-right of titlebar
    nisaba::Rect rect_theme_toggle_{};
    bool theme_toggle_hovered_{false};
    bool theme_toggle_pressed_{false};

    // Navigation & Page State: 0=Processes, 1=CPU, 2=Memory, 3=Network, 4=Disk, 5=GPU
    int current_page_{0};
    int hovered_sidebar_item_{-1};
    std::vector<nisaba::Rect> sidebar_rects_;

    // Processes Page State
    SelectionTarget selection_{};
    bool search_focused_{false};
    float cursor_blink_{0.0f};
    float proc_scroll_y_{0.0f};
    float proc_target_scroll_y_{0.0f};
    float proc_max_scroll_y_{0.0f};
    float proc_scrollbar_opacity_{0.0f};
    float proc_scrollbar_timer_{0.0f};
    int hovered_proc_row_{-1};
    int hovered_scope_seg_{-1};
    int pressed_scope_seg_{-1};
    int hovered_sort_col_{-1};
    bool quit_btn_hovered_{false};
    bool quit_btn_pressed_{false};

    // SegmentedControl spring glide animation (kGlideResponse = 0.3, kStretch = 0.08)
    float seg_plate_pos_{1.0f};
    float seg_plate_vel_{0.0f};
    float seg_plate_stretch_{0.0f};

    nisaba::Rect rect_scope_bar_{};
    nisaba::Rect rect_scope_segs_[3]{};
    nisaba::Rect rect_search_box_{};
    nisaba::Rect rect_search_clear_{};
    nisaba::Rect rect_quit_btn_{};
    nisaba::Rect rect_table_{};
    nisaba::Rect rect_table_body_{};
    nisaba::Rect rect_col_headers_[6]{};

    // Double-click detection on process rows
    std::chrono::steady_clock::time_point last_row_click_time_{};
    int last_clicked_pid_{-1};

    // Resource Page State (Pages 1..5)
    float res_scroll_y_{0.0f};
    float res_target_scroll_y_{0.0f};
    float res_max_scroll_y_{0.0f};
    float res_scrollbar_opacity_{0.0f};
    float res_scrollbar_timer_{0.0f};

    // Context Menu Overlay State (FishUI Menu metrics)
    bool ctx_menu_open_{false};
    float ctx_menu_x_{0.0f};
    float ctx_menu_y_{0.0f};
    int ctx_menu_hovered_{-1};
    nisaba::Rect rect_ctx_menu_{};
    nisaba::Rect rect_ctx_items_[4]{};

    // Process / Application Details Dialog State (FishUI DialogWindow 480px width)
    bool details_open_{false};
    SelectionTarget details_target_{};
    int64_t boot_time_{0};
    float details_members_scroll_y_{0.0f};
    float details_members_max_scroll_y_{0.0f};
    nisaba::Rect rect_details_modal_{};
    nisaba::Rect rect_details_close_{};
    nisaba::Rect rect_details_members_area_{};
    bool details_close_hovered_{false};
    bool details_close_pressed_{false};

    // Confirm End / Force Quit Alert Dialog State (FishUI ConfirmDialog / AlertView)
    bool confirm_open_{false};
    bool confirm_force_{false};
    SelectionTarget confirm_target_{};
    nisaba::Rect rect_confirm_modal_{};
    nisaba::Rect rect_confirm_cancel_{};
    nisaba::Rect rect_confirm_accept_{};
    int confirm_hovered_btn_{-1};
    int confirm_pressed_btn_{-1};

    // Toast Notification State (FishUI Toast)
    std::string toast_text_;
    float toast_timer_{0.0f};

    bool should_quit_{false};
    float mouse_x_{-1.0f};
    float mouse_y_{-1.0f};

    // Helpers
    ProcessList& active_list();
    const ProcessRecord* selected_record() const;
    const ProcessRecord* find_record(const SelectionTarget& target) const;
    void select_row(size_t row);
    void clear_selection();
    void scroll_to_row(size_t row);
    void open_details_modal();
    void open_confirm_modal(bool force);
    void show_toast(std::string msg);
    std::string format_running_time(const ProcessRecord& proc) const;

    // Sub-renderers
    void render_sidebar(nisaba::gpu::GpuCanvas& canvas, float sidebar_w);
    void render_processes_page(nisaba::gpu::GpuCanvas& canvas, const nisaba::Rect& page_rect);
    void render_resource_page(nisaba::gpu::GpuCanvas& canvas, const nisaba::Rect& page_rect);
    void render_context_menu(nisaba::gpu::GpuCanvas& canvas);
    void render_details_modal(nisaba::gpu::GpuCanvas& canvas);
    void render_confirm_modal(nisaba::gpu::GpuCanvas& canvas);
    void render_toast(nisaba::gpu::GpuCanvas& canvas);
};

} // namespace sysmon
