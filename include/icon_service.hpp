#pragma once
/// @file icon_service.hpp
/// @brief Sovereign Application Icon Discovery, SVG/PNG Decoding & GPU Texture Service.
/// Ported directly from Obsidian (`/home/x/Desktop/obsidian/src/services/app_service.cpp`)
/// and expanded with full XDG / user icon theme and local application bundle support.

#include "nisaba/color/color.hpp"
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/gpu/gpu_canvas.hpp"
#include "nisaba/gpu/gpu_device.hpp"
#include "nisaba/gpu/gpu_texture.hpp"
#include "nisaba/image/image_io.hpp"
#include "nisaba/svg/svg.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sysmon {

class IconService {
public:
    static IconService& instance() {
        static IconService s_instance;
        return s_instance;
    }

    /// Resolves an icon name or path across all Obsidian + XDG + User icon themes
    [[nodiscard]] std::string resolve_icon_path(std::string_view icon_name) {
        if (icon_name.empty()) return "";
        std::lock_guard<std::mutex> lock(mutex_);

        std::string key(icon_name);
        auto it = path_cache_.find(key);
        if (it != path_cache_.end()) return it->second;

        namespace fs = std::filesystem;
        std::error_code ec;

        // 1. Direct absolute path check
        if (key[0] == '/' && fs::exists(key, ec) && fs::is_regular_file(key, ec)) {
            path_cache_[key] = key;
            return key;
        }

        auto to_lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return s;
        };

        std::vector<std::string> candidates;
        candidates.push_back(key);

        std::string stem = fs::path(key).stem().string();
        if (!stem.empty() && stem != key) {
            candidates.push_back(stem);
            candidates.push_back(to_lower(stem));
        }

        std::string lower = to_lower(key);
        if (lower != key) {
            candidates.push_back(lower);
        }

        // Reverse-DNS short name (e.g. org.vaxp.vater -> vater, org.gnome.Nautilus -> nautilus)
        auto dot_pos = key.rfind('.');
        if (dot_pos != std::string::npos && dot_pos + 1 < key.size()) {
            std::string short_name = key.substr(dot_pos + 1);
            candidates.push_back(short_name);
            candidates.push_back(to_lower(short_name));
        }

        // Common standard fallbacks from Obsidian AppService
        if (lower.find("terminal") != std::string::npos || lower == "vater") {
            candidates.push_back("vater");
            candidates.push_back("utilities-terminal");
            candidates.push_back("terminal");
        }
        if (lower.find("chrome") != std::string::npos) {
            candidates.push_back("google-chrome");
            candidates.push_back("chromium-browser");
        }
        if (lower.find("code") != std::string::npos || lower.find("antigravity") != std::string::npos) {
            candidates.push_back("vscode");
            candidates.push_back("code");
            candidates.push_back("vcodex");
        }
        if (lower.find("calculator") != std::string::npos) {
            candidates.push_back("accessories-calculator");
            candidates.push_back("calculator");
        }
        if (lower.find("text-editor") != std::string::npos || lower.find("gedit") != std::string::npos) {
            candidates.push_back("accessories-text-editor");
            candidates.push_back("text-editor");
        }
        if (lower.find("files") != std::string::npos || lower.find("nautilus") != std::string::npos) {
            candidates.push_back("aetherfiles");
            candidates.push_back("system-file-manager");
            candidates.push_back("nautilus");
        }
        if (lower.find("settings") != std::string::npos) {
            candidates.push_back("preferences-system");
            candidates.push_back("settings");
        }
        if (lower.find("monitor") != std::string::npos || lower.find("taskmanager") != std::string::npos) {
            candidates.push_back("utilities-system-monitor");
            candidates.push_back("vtaskmanager");
        }
        if (lower.find("picom") != std::string::npos || lower.find("compton") != std::string::npos ||
            lower.find("asgard") != std::string::npos || lower.find("obsidian") != std::string::npos) {
            candidates.push_back("preferences-system-windows");
            candidates.push_back("cs-windows");
        }

        const auto& dirs = icon_search_dirs();
        for (const auto& cand : candidates) {
            for (const auto& dir : dirs) {
                if (!fs::exists(dir, ec)) continue;

                fs::path p1 = fs::path(dir) / cand;
                if (fs::exists(p1, ec) && fs::is_regular_file(p1, ec)) {
                    return path_cache_[key] = p1.string();
                }

                fs::path p_svg = fs::path(dir) / (cand + ".svg");
                if (fs::exists(p_svg, ec) && fs::is_regular_file(p_svg, ec)) {
                    return path_cache_[key] = p_svg.string();
                }

                fs::path p_png = fs::path(dir) / (cand + ".png");
                if (fs::exists(p_png, ec) && fs::is_regular_file(p_png, ec)) {
                    return path_cache_[key] = p_png.string();
                }

                fs::path p_webp = fs::path(dir) / (cand + ".webp");
                if (fs::exists(p_webp, ec) && fs::is_regular_file(p_webp, ec)) {
                    return path_cache_[key] = p_webp.string();
                }

                fs::path p_sym = fs::path(dir) / (cand + "-symbolic.svg");
                if (fs::exists(p_sym, ec) && fs::is_regular_file(p_sym, ec)) {
                    return path_cache_[key] = p_sym.string();
                }
            }
        }

        path_cache_[key] = "";
        return "";
    }

    /// Loads an icon file (.svg, .png, .webp, or SVG saved with .png extension) into a Nisaba Pixmap
    /// Exact port of Obsidian's `AppService::load_icon_pixmap`.
    [[nodiscard]] std::shared_ptr<nisaba::Pixmap> load_icon_pixmap(const std::string& path) {
        if (path.empty()) return nullptr;
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(path, ec) || !fs::is_regular_file(path, ec)) return nullptr;

        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        auto try_svg = [&]() -> std::shared_ptr<nisaba::Pixmap> {
            auto doc = nisaba::svg::SvgDocument::from_file(path);
            if (doc.has_value()) {
                auto baked = nisaba::svg::BakedSvg::bake(*doc, 128, 128);
                if (baked.is_valid()) {
                    return std::make_shared<nisaba::Pixmap>(baked.pixmap());
                }
            }
            return nullptr;
        };

        auto try_raster = [&]() -> std::shared_ptr<nisaba::Pixmap> {
            auto img = nisaba::image::load_image_file(path);
            if (img.has_value()) {
                return std::make_shared<nisaba::Pixmap>(std::move(*img));
            }
            return nullptr;
        };

        if (ext == ".svg") {
            if (auto pix = try_svg()) return pix;
            return try_raster();
        }

        // Many desktop icons (e.g. VAXP suite in /usr/share/icons/hicolor/256x256/apps/*.png)
        // are actually XML SVG documents saved with a .png extension.
        // Try raster decoding first; if magic-byte inspection rejects it, fall back to SVG baking.
        if (auto pix = try_raster()) return pix;
        return try_svg();
    }

    /// Retrieves or uploads a hardware GpuTexture for an icon name/path
    /// Exact port of Obsidian's `AppService::get_icon_texture`.
    [[nodiscard]] std::shared_ptr<nisaba::gpu::GpuTexture> get_icon_texture(
        std::string_view icon_name,
        const std::shared_ptr<nisaba::gpu::GpuDevice>& device)
    {
        if (!device || icon_name.empty()) return nullptr;
        std::string key(icon_name);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto tex_it = texture_cache_.find(key);
            if (tex_it != texture_cache_.end()) {
                return tex_it->second;
            }
        }

        std::string resolved = resolve_icon_path(key);
        if (resolved.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            texture_cache_[key] = nullptr;
            return nullptr;
        }

        std::shared_ptr<nisaba::Pixmap> pix;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto pix_it = pixmap_cache_.find(resolved);
            if (pix_it != pixmap_cache_.end()) {
                pix = pix_it->second;
            }
        }

        if (!pix) {
            pix = load_icon_pixmap(resolved);
            std::lock_guard<std::mutex> lock(mutex_);
            pixmap_cache_[resolved] = pix;
        }

        if (pix && pix->width() > 0 && pix->height() > 0) {
            auto tex = nisaba::gpu::GpuTexture::create_from_pixmap(
                device, pix->as_ref(), nisaba::gpu::GpuTextureFilter::Linear);
            std::lock_guard<std::mutex> lock(mutex_);
            texture_cache_[key] = tex;
            return tex;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        texture_cache_[key] = nullptr;
        return nullptr;
    }

    /// Retrieves a cached Pixmap for an icon name (used if GpuDevice is not available)
    [[nodiscard]] std::shared_ptr<nisaba::Pixmap> get_icon_pixmap(std::string_view icon_name) {
        if (icon_name.empty()) return nullptr;
        std::string resolved = resolve_icon_path(icon_name);
        if (resolved.empty()) return nullptr;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pixmap_cache_.find(resolved);
            if (it != pixmap_cache_.end()) return it->second;
        }

        auto pix = load_icon_pixmap(resolved);
        std::lock_guard<std::mutex> lock(mutex_);
        pixmap_cache_[resolved] = pix;
        return pix;
    }

private:
    IconService() = default;

    static const std::vector<std::string>& icon_search_dirs() {
        static const std::vector<std::string> dirs = []() {
            std::vector<std::string> list;
            const char* home_env = std::getenv("HOME");
            std::string home = home_env ? home_env : "/home/x";

            // User local icon themes (WhiteSur, hicolor, etc.)
            list.push_back(home + "/.local/share/icons/WhiteSur/apps/scalable");
            list.push_back(home + "/.local/share/icons/WhiteSur/mimes/scalable");
            list.push_back(home + "/.local/share/icons/WhiteSur/devices/scalable");
            list.push_back(home + "/.local/share/icons/WhiteSur/places/scalable");
            list.push_back(home + "/.local/share/icons/WhiteSur/categories/scalable");
            list.push_back(home + "/.local/share/icons/WhiteSur-dark/apps/scalable");
            list.push_back(home + "/.local/share/icons/WhiteSur-light/apps/scalable");
            list.push_back(home + "/.local/share/icons/hicolor/scalable/apps");
            list.push_back(home + "/.local/share/icons/hicolor/256x256/apps");
            list.push_back(home + "/.local/share/icons/hicolor/128x128/apps");
            list.push_back(home + "/.local/share/icons/hicolor/64x64/apps");
            list.push_back(home + "/.local/share/icons/hicolor/48x48/apps");

            // Obsidian's full ICON_DIRS list
            const char* const sys_dirs[] = {
                "/usr/share/icons/Nordzy-dark--light_panel/apps/scalable",
                "/usr/share/icons/Nordzy-dark--light_panel/apps/symbolic",
                "/usr/share/icons/Nordzy-dark--light_panel/mimes/scalable",
                "/usr/share/icons/Nordzy-dark--light_panel/devices/scalable",
                "/usr/share/icons/Nordzy-dark--light_panel/places/scalable",
                "/usr/share/icons/Nordzy-dark--light_panel/categories/scalable",
                "/usr/share/icons/Nordzy-dark--light_panel/status/24",
                "/usr/share/icons/Nordzy--dark_panel/apps/scalable",
                "/usr/share/icons/Nordzy--dark_panel/mimes/scalable",
                "/usr/share/icons/Nordzy--dark_panel/devices/scalable",
                "/usr/share/icons/hicolor/scalable/apps",
                "/usr/share/icons/hicolor/512x512/apps",
                "/usr/share/icons/hicolor/256x256/apps",
                "/usr/share/icons/hicolor/128x128/apps",
                "/usr/share/icons/hicolor/96x96/apps",
                "/usr/share/icons/hicolor/64x64/apps",
                "/usr/share/icons/hicolor/48x48/apps",
                "/usr/share/icons/hicolor/32x32/apps",
                "/usr/share/icons/hicolor/symbolic/apps",
                "/usr/share/icons/Adwaita/scalable/apps",
                "/usr/share/icons/Adwaita/scalable/mimetypes",
                "/usr/share/icons/Adwaita/scalable/devices",
                "/usr/share/icons/Adwaita/scalable/places",
                "/usr/share/icons/Adwaita/scalable/categories",
                "/usr/share/icons/Adwaita/scalable/status",
                "/usr/share/icons/Adwaita/48x48/apps",
                "/usr/share/icons/Adwaita/symbolic/apps",
                "/usr/share/icons/Yaru/scalable/apps",
                "/usr/share/icons/Yaru/48x48/apps",
                "/usr/share/icons/Papirus/48x48/apps",
                "/usr/share/icons/Papirus/scalable/apps",
                "/usr/share/icons/gnome/48x48/apps",
                "/usr/share/icons/gnome/scalable/apps",
                "/usr/share/icons/Humanity/apps/48",
                "/usr/share/icons/Humanity/mimes/48",
                "/usr/share/icons/Humanity/devices/48",
                "/usr/share/icons/ubuntu-mono-dark/apps/48",
                "/usr/share/pixmaps",
                "/var/lib/flatpak/exports/share/icons/hicolor/scalable/apps",
                "/var/lib/flatpak/exports/share/icons/hicolor/128x128/apps",
                "/var/lib/snapd/desktop/assets",
            };
            for (const char* d : sys_dirs) {
                list.emplace_back(d);
            }
            return list;
        }();
        return dirs;
    }

    std::mutex mutex_;
    std::unordered_map<std::string, std::string> path_cache_;
    std::unordered_map<std::string, std::shared_ptr<nisaba::Pixmap>> pixmap_cache_;
    std::unordered_map<std::string, std::shared_ptr<nisaba::gpu::GpuTexture>> texture_cache_;
};

} // namespace sysmon

