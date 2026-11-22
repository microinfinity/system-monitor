#pragma once
/// @file vector_icons.hpp
/// @brief Sovereign SVG Icon Engine & Desktop Icon Loader for Obsidian System Monitor on Nisaba.
/// Uses `nisaba::svg::SvgDocument` and `nisaba::svg::SvgCache` directly to render the original
/// project SVG files (`icons/*.svg`, `window-control-*.svg`, `search.svg`, `clear.svg`, `chevron-*.svg`)
/// and system desktop application icons (`.svg` / `.png`).

#include "nisaba/gpu/gpu_canvas.hpp"
#include "nisaba/canvas/paint.hpp"
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/svg/svg.hpp"
#include "nisaba/math/rect.hpp"
#include "icon_service.hpp"
#include "theme.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sysmon {

inline float measure_text(nisaba::gpu::GpuCanvas& canvas, std::string_view text, float font_size, const char* font_name = "sans") {
    if (text.empty()) return 0.0f;
    if (auto* ctx = canvas.context()) {
        ctx->fontSize(font_size);
        ctx->fontFace(font_name ? font_name : "sans");
        float bounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        return ctx->textBounds(0.0f, 0.0f, text.data(), text.data() + text.size(), bounds);
    }
    return static_cast<float>(text.size()) * font_size * 0.54f;
}

inline std::string elide_right(nisaba::gpu::GpuCanvas& canvas, std::string_view text, float max_w, float font_size, const char* font_name = "sans") {
    if (max_w <= 4.0f || text.empty()) return "";
    if (measure_text(canvas, text, font_size, font_name) <= max_w) {
        return std::string(text);
    }
    const std::string ell = "…";
    float ell_w = measure_text(canvas, ell, font_size, font_name);
    if (ell_w >= max_w) return "";
    size_t len = text.size();
    while (len > 0) {
        --len;
        while (len > 0 && (static_cast<uint8_t>(text[len]) & 0xC0) == 0x80) {
            --len;
        }
        std::string sub = std::string(text.substr(0, len)) + ell;
        if (measure_text(canvas, sub, font_size, font_name) <= max_w) {
            return sub;
        }
    }
    return ell;
}

inline std::string elide_middle(nisaba::gpu::GpuCanvas& canvas, std::string_view text, float max_w, float font_size, const char* font_name = "sans") {
    if (max_w <= 4.0f || text.empty()) return "";
    if (measure_text(canvas, text, font_size, font_name) <= max_w) {
        return std::string(text);
    }
    const std::string ell = "…";
    if (text.size() < 4) return elide_right(canvas, text, max_w, font_size, font_name);
    size_t left_len = text.size() / 2;
    size_t right_len = text.size() - left_len;
    while (left_len > 1 || right_len > 1) {
        if (left_len >= right_len && left_len > 1) {
            --left_len;
            while (left_len > 0 && (static_cast<uint8_t>(text[left_len]) & 0xC0) == 0x80) --left_len;
        } else if (right_len > 1) {
            --right_len;
            while (right_len > 0 && (static_cast<uint8_t>(text[text.size() - right_len]) & 0xC0) == 0x80) --right_len;
        } else {
            break;
        }
        std::string cand = std::string(text.substr(0, left_len)) + ell + std::string(text.substr(text.size() - right_len));
        if (measure_text(canvas, cand, font_size, font_name) <= max_w) {
            return cand;
        }
    }
    return ell;
}

/// Official SVG assets from `system-monitor/icons/*.svg` and `obsidian-toolkit/data/icons/*.svg`
namespace svg_assets {

inline constexpr std::string_view kProcessesSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16" fill="none"><g stroke="currentColor" stroke-linecap="round" stroke-linejoin="round"><rect x="1.5" y="2.5" width="13" height="11" rx="2"/><path d="M6.5 5.5h5M6.5 8h5M6.5 10.5h5"/></g><g fill="currentColor"><circle cx="4.5" cy="5.5" r="0.75"/><circle cx="4.5" cy="8" r="0.75"/><circle cx="4.5" cy="10.5" r="0.75"/></g></svg>)svg";

inline constexpr std::string_view kCpuSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16" fill="none"><g stroke="currentColor" stroke-linecap="round" stroke-linejoin="round"><rect x="3.5" y="3.5" width="9" height="9" rx="2"/><rect x="6.5" y="6.5" width="3" height="3" rx="0.5"/><path d="M6.5 1.5v2M9.5 1.5v2M6.5 12.5v2M9.5 12.5v2M1.5 6.5h2M1.5 9.5h2M12.5 6.5h2M12.5 9.5h2"/></g></svg>)svg";

inline constexpr std::string_view kMemorySvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16" fill="none"><g stroke="currentColor" stroke-linecap="round" stroke-linejoin="round"><rect x="1.5" y="3.5" width="13" height="7" rx="1.5"/><path d="M3.5 10.5v2M6.5 10.5v2M9.5 10.5v2M12.5 10.5v2"/></g><g fill="currentColor"><rect x="4" y="6" width="2" height="2"/><rect x="7" y="6" width="2" height="2"/><rect x="10" y="6" width="2" height="2"/></g></svg>)svg";

inline constexpr std::string_view kNetworkSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16" fill="none"><g stroke="currentColor" stroke-linecap="round" stroke-linejoin="round"><circle cx="8" cy="8" r="6.5"/><ellipse cx="8" cy="8" rx="2.5" ry="6.5"/><path d="M1.5 8h13"/></g></svg>)svg";

inline constexpr std::string_view kDiskSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16" fill="none"><g stroke="currentColor" stroke-linecap="round" stroke-linejoin="round"><rect x="1.5" y="2.5" width="13" height="11" rx="2"/><path d="M1.5 9.5h13M4.5 11.5h3"/></g><circle cx="11.5" cy="11.5" r="1" fill="currentColor"/></svg>)svg";

inline constexpr std::string_view kGpuSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16" fill="none"><g stroke="currentColor" stroke-linecap="round" stroke-linejoin="round"><path d="M1.5 2.5h1a1 1 0 0 1 1 1v11"/><path d="M3.5 4.5H13a1.5 1.5 0 0 1 1.5 1.5v4a1.5 1.5 0 0 1-1.5 1.5H3.5"/><circle cx="10" cy="8" r="2"/><path d="M6.5 11.5v2h4v-2"/></g></svg>)svg";

inline constexpr std::string_view kQuitSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16" fill="none"><g stroke="currentColor" stroke-linecap="round" stroke-linejoin="round"><path d="M5.31 1.5h5.38l3.81 3.81v5.38l-3.81 3.81H5.31L1.5 10.69V5.31Z"/><path d="M5.75 5.75l4.5 4.5M10.25 5.75l-4.5 4.5"/></g></svg>)svg";

inline constexpr std::string_view kSearchSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16" fill="none"><g stroke="currentColor" stroke-linecap="round"><circle cx="7" cy="7" r="4.5"/><path d="M10.5 10.5l3 3"/></g></svg>)svg";

inline constexpr std::string_view kClearSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16"><path d="M8 1a7 7 0 1 0 0 14A7 7 0 1 0 8 1zM9.77 5.17 10.8 6.23 9.06 8 10.8 9.77 9.77 10.8 8 9.06 6.23 10.8 5.17 9.77 6.94 8 5.17 6.23 6.23 5.17 8 6.94Z" fill="currentColor" fill-rule="evenodd"/></svg>)svg";

inline constexpr std::string_view kChevronUpSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="12" height="12" viewBox="0 0 12 12" fill="none"><path d="M2.5 7.5L6 4l3.5 3.5" stroke="currentColor" stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5"/></svg>)svg";

inline constexpr std::string_view kChevronDownSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="12" height="12" viewBox="0 0 12 12" fill="none"><path d="M2.5 4.5L6 8l3.5-3.5" stroke="currentColor" stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5"/></svg>)svg";

inline constexpr std::string_view kWindowControlCloseSvg = R"svg(<svg width="14" height="14" viewBox="0 0 14 14" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M3.967 3.967L10.033 10.033M10.033 3.967L3.967 10.033" stroke="currentColor" stroke-width="1.12" stroke-linecap="round"/></svg>)svg";

inline constexpr std::string_view kWindowControlMinimizeSvg = R"svg(<svg width="14" height="14" viewBox="0 0 14 14" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M3.267 6.4H10.733" stroke="currentColor" stroke-width="1.12" stroke-linecap="round"/></svg>)svg";

inline constexpr std::string_view kWindowControlMaximizeSvg = R"svg(<svg width="14" height="14" viewBox="0 0 14 14" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M7.5 3.6H10.4V6.5M6.5 10.4H3.6V7.5" stroke="currentColor" stroke-width="1.12" stroke-linecap="round" stroke-linejoin="round"/></svg>)svg";

inline constexpr std::string_view kWindowControlRestoreSvg = R"svg(<svg width="14" height="14" viewBox="0 0 14 14" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M8.45 3.05V5.55H10.95M5.55 10.95V8.45H3.05" stroke="currentColor" stroke-width="1.2" stroke-linecap="round" stroke-linejoin="round"/></svg>)svg";

inline constexpr std::string_view kWindowControlDialogCloseSvg = R"svg(<svg width="22" height="22" viewBox="0 0 22 22" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M7.75 7.75L14.25 14.25M14.25 7.75L7.75 14.25" stroke="currentColor" stroke-width="1.25" stroke-linecap="round"/></svg>)svg";

inline constexpr std::string_view kSunSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 14 14" fill="none"><g stroke="currentColor" stroke-width="1.15" stroke-linecap="round"><circle cx="7" cy="7" r="2.6"/><path d="M7 1.6v1.2M7 11.2v1.2M1.6 7h1.2M11.2 7h1.2M3.18 3.18l.85.85M9.97 9.97l.85.85M10.82 3.18l-.85.85M4.03 9.97l-.85.85"/></g></svg>)svg";

inline constexpr std::string_view kMoonSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 14 14" fill="none"><path d="M10.8 8.75A4.5 4.5 0 0 1 5.25 3.2a4.5 4.5 0 1 0 5.55 5.55Z" stroke="currentColor" stroke-width="1.15" stroke-linecap="round" stroke-linejoin="round"/></svg>)svg";

} // namespace svg_assets

class VectorIcons {
public:
    static nisaba::svg::SvgCache& svg_cache() {
        static nisaba::svg::SvgCache cache(512);
        return cache;
    }

    static const nisaba::svg::SvgDocument* get_builtin_svg(std::string_view key, std::string_view fallback_xml) {
        static std::unordered_map<std::string, nisaba::svg::SvgDocument> docs;
        std::string k(key);
        auto it = docs.find(k);
        if (it != docs.end()) return &it->second;

        // First check if file exists on disk (e.g. icons/<name>.svg)
        namespace fs = std::filesystem;
        std::error_code ec;
        const std::string disk_candidates[] = {
            "icons/" + k + ".svg",
            "../icons/" + k + ".svg",
            "/usr/share/system-monitor/icons/" + k + ".svg"
        };
        for (const auto& path : disk_candidates) {
            if (fs::exists(path, ec) && fs::is_regular_file(path, ec)) {
                if (auto doc = nisaba::svg::SvgDocument::from_file(path)) {
                    auto [ins, _] = docs.emplace(std::move(k), std::move(*doc));
                    return &ins->second;
                }
            }
        }

        if (auto doc = nisaba::svg::SvgDocument::parse(fallback_xml)) {
            auto [ins, _] = docs.emplace(std::move(k), std::move(*doc));
            return &ins->second;
        }
        return nullptr;
    }

    static void draw_svg_icon(nisaba::gpu::GpuCanvas& canvas, std::string_view key, std::string_view fallback_xml,
                              float cx, float cy, float r, nisaba::Color col) {
        const auto* doc = get_builtin_svg(key, fallback_xml);
        if (!doc) return;
        auto bounds = *nisaba::Rect::from_xywh(cx - r, cy - r, r * 2.0f, r * 2.0f);
        nisaba::svg::SvgTheme svg_theme;
        svg_theme.current_color = col;
        svg_theme.tint_color = col;
        svg_cache().draw(canvas, *doc, bounds, svg_theme, 1.0f);
    }

    static void draw_processes(nisaba::gpu::GpuCanvas& canvas, float cx, float cy, float r, nisaba::Color col) {
        draw_svg_icon(canvas, "processes", svg_assets::kProcessesSvg, cx, cy, r, col);
    }

    static void draw_cpu(nisaba::gpu::GpuCanvas& canvas, float cx, float cy, float r, nisaba::Color col) {
        draw_svg_icon(canvas, "cpu", svg_assets::kCpuSvg, cx, cy, r, col);
    }

    static void draw_memory(nisaba::gpu::GpuCanvas& canvas, float cx, float cy, float r, nisaba::Color col) {
        draw_svg_icon(canvas, "memory", svg_assets::kMemorySvg, cx, cy, r, col);
    }

    static void draw_network(nisaba::gpu::GpuCanvas& canvas, float cx, float cy, float r, nisaba::Color col) {
        draw_svg_icon(canvas, "network", svg_assets::kNetworkSvg, cx, cy, r, col);
    }

    static void draw_disk(nisaba::gpu::GpuCanvas& canvas, float cx, float cy, float r, nisaba::Color col) {
        draw_svg_icon(canvas, "disk", svg_assets::kDiskSvg, cx, cy, r, col);
    }

    static void draw_gpu(nisaba::gpu::GpuCanvas& canvas, float cx, float cy, float r, nisaba::Color col) {
        draw_svg_icon(canvas, "gpu", svg_assets::kGpuSvg, cx, cy, r, col);
    }

    static void draw_quit(nisaba::gpu::GpuCanvas& canvas, float cx, float cy, float r, nisaba::Color col) {
        draw_svg_icon(canvas, "quit", svg_assets::kQuitSvg, cx, cy, r, col);
    }

    static void draw_search(nisaba::gpu::GpuCanvas& canvas, float cx, float cy, float r, nisaba::Color col) {
        draw_svg_icon(canvas, "search", svg_assets::kSearchSvg, cx, cy, r, col);
    }

    static void draw_clear(nisaba::gpu::GpuCanvas& canvas, float cx, float cy, float r, nisaba::Color col, nisaba::Color /*bg_cutout*/) {
        draw_svg_icon(canvas, "clear", svg_assets::kClearSvg, cx, cy, r, col);
    }

    static void draw_chevron(nisaba::gpu::GpuCanvas& canvas, float cx, float cy, float r, bool up, nisaba::Color col) {
        if (up) {
            draw_svg_icon(canvas, "chevron-up", svg_assets::kChevronUpSvg, cx, cy, r, col);
        } else {
            draw_svg_icon(canvas, "chevron-down", svg_assets::kChevronDownSvg, cx, cy, r, col);
        }
    }

    static void draw_window_control_glyph(nisaba::gpu::GpuCanvas& canvas, std::string_view artwork,
                                          const nisaba::Rect& bounds, nisaba::Color col) {
        if (artwork.empty()) return;
        std::string key = "window-control-" + std::string(artwork);
        std::string_view xml = svg_assets::kWindowControlCloseSvg;
        if (artwork == "close") xml = svg_assets::kWindowControlCloseSvg;
        else if (artwork == "minimize") xml = svg_assets::kWindowControlMinimizeSvg;
        else if (artwork == "maximize") xml = svg_assets::kWindowControlMaximizeSvg;
        else if (artwork == "restore") xml = svg_assets::kWindowControlRestoreSvg;
        else if (artwork == "dialog-close") xml = svg_assets::kWindowControlDialogCloseSvg;
        else return;

        const auto* doc = get_builtin_svg(key, xml);
        if (!doc) return;
        nisaba::svg::SvgTheme svg_theme;
        svg_theme.current_color = col;
        svg_theme.tint_color = col;
        svg_cache().draw(canvas, *doc, bounds, svg_theme, 1.0f);
    }

    static void draw_theme_toggle(nisaba::gpu::GpuCanvas& canvas, float cx, float cy, float r, bool dark_mode, nisaba::Color col) {
        if (dark_mode) {
            draw_svg_icon(canvas, "theme-moon", svg_assets::kMoonSvg, cx, cy, r, col);
        } else {
            draw_svg_icon(canvas, "theme-sun", svg_assets::kSunSvg, cx, cy, r, col);
        }
    }

    static void draw_process_badge(nisaba::gpu::GpuCanvas& canvas, float cx, float cy, float size,
                                   std::string_view icon_name, std::string_view display_name,
                                   const AppTheme& /*theme*/, bool /*selected*/) {
        float half = size * 0.5f;
        auto box = *nisaba::Rect::from_xywh(cx - half, cy - half, size, size);

        std::string_view lookup_name = icon_name.empty() ? "application-x-executable" : icon_name;

        // 1. Hardware GPU Texture path via Obsidian IconService (supports .svg, .png, .webp, and SVG-in-PNG)
        std::shared_ptr<nisaba::gpu::GpuDevice> gpu_dev =
            canvas.surface() ? canvas.surface()->device() : nullptr;

        if (gpu_dev) {
            auto tex = IconService::instance().get_icon_texture(lookup_name, gpu_dev);
            if (!tex && lookup_name != display_name && !display_name.empty()) {
                tex = IconService::instance().get_icon_texture(display_name, gpu_dev);
            }
            if (tex && tex->width() > 0 && tex->height() > 0) {
                canvas.save();
                canvas.translate(box.left(), box.top());
                canvas.scale(size / static_cast<float>(tex->width()),
                             size / static_cast<float>(tex->height()));
                canvas.draw_gpu_texture(0, 0, tex);
                canvas.restore();
                return;
            }
        } else {
            auto pix = IconService::instance().get_icon_pixmap(lookup_name);
            if (pix && pix->width() > 0 && pix->height() > 0) {
                auto src_r = *nisaba::Rect::from_xywh(0.0f, 0.0f, static_cast<float>(pix->width()), static_cast<float>(pix->height()));
                canvas.draw_pixmap_rect(pix->as_ref(), src_r, box);
                return;
            }
        }

        // 2. Fallback application icon badge with Obsidian accent colors if icon is not found
        uint32_t h = 2166136261u;
        for (char c : display_name) {
            h ^= static_cast<uint8_t>(c);
            h *= 16777619u;
        }
        static const nisaba::Color kAppColors[] = {
            accent::blue,
            accent::red,
            accent::green,
            accent::purple,
            accent::orange,
            accent::pink,
            accent::grey
        };
        nisaba::Color acc = kAppColors[h % 7];
        canvas.fill_round_rect(box, size * 0.25f, size * 0.25f, nisaba::Paint(acc));

        char initial = 'A';
        for (char c : display_name) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                initial = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
                break;
            }
        }
        std::string s(1, initial);
        float fsz = size * 0.52f;
        float tw = measure_text(canvas, s, fsz);
        canvas.draw_text(cx - tw * 0.5f, cy + fsz * 0.36f, s,
                         nisaba::Paint(nisaba::Color::WHITE), fsz, "sans");
    }
};

} // namespace sysmon
