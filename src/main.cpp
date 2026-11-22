/// @file main.cpp
/// @brief Obsidian System Monitor — Native C++20 GPU Wayland/X11 Application on Nisaba Engine.

#include "nisaba/gpu/gpu_device.hpp"
#include "nisaba/gpu/gpu_surface.hpp"
#include "nisaba/gpu/gpu_canvas.hpp"
#include "nisaba/color/color.hpp"

#ifdef NISABA_HAS_BACKEND_OS
#   include "nisaba/backend_os/platform.hpp"
#   include "nisaba/backend_os/window.hpp"
#endif

#ifdef NISABA_GLEW
#   include <GL/glew.h>
#else
#   include <GL/gl.h>
#endif

#include "embedded_font.hpp"
#include "system_telemetry.hpp"
#include "system_monitor_app.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifndef SYSTEM_MONITOR_VERSION
#   define SYSTEM_MONITOR_VERSION "1.0"
#endif

using namespace nisaba;
using nisaba::gpu::GpuDevice;
using nisaba::gpu::GpuSurface;
using nisaba::gpu::GpuCanvas;

static int parseArguments(int argc, char** argv, bool* quit, bool* dark_mode) {
    *quit = false;
    *dark_mode = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-h" || argument == "--help" || argument == "-?") {
            std::printf("Usage: %s [options]\nObsidian System Monitor (Nisaba Engine)\n\nOptions:\n"
                        "  -h, --help     Displays help on commandline options.\n"
                        "  -v, --version  Displays version information.\n"
                        "  --dark         Start in Obsidian Dark Mode.\n"
                        "  --light        Start in Obsidian Light Mode (default).\n",
                        argv[0]);
            *quit = true;
            return 0;
        }
        if (argument == "-v" || argument == "--version") {
            std::printf("system-monitor %s (Nisaba Engine)\n", SYSTEM_MONITOR_VERSION);
            *quit = true;
            return 0;
        }
        if (argument == "--dark") {
            *dark_mode = true;
            continue;
        }
        if (argument == "--light") {
            *dark_mode = false;
            continue;
        }
        std::fprintf(stderr, "Unknown option '%s'.\n",
                     argument.substr(argument.find_first_not_of('-')).c_str());
        *quit = true;
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    bool quit = false;
    bool dark_mode = false;
    const int status = parseArguments(argc, argv, &quit, &dark_mode);
    if (quit) return status;

    std::cout << "========================================================\n"
              << "  Obsidian System Monitor — 1:1 Replica on Nisaba Engine\n"
              << "========================================================\n";

#ifdef NISABA_HAS_BACKEND_OS
    auto plat_res = backend_os::Platform::create();
    if (!plat_res.isOk()) {
        std::cerr << "[-] Failed to initialize native OS platform\n";
        return 1;
    }
    auto platform = std::move(plat_res.value());

    uint32_t win_w = 907;
    uint32_t win_h = 640;

    backend_os::WindowConfig wcfg;
    wcfg.title = "System Monitor";
    wcfg.app_id = "system-monitor";
    wcfg.vsync = true;
    wcfg.transparent = true;
    wcfg.blur = true;
    wcfg.width = static_cast<int>(win_w);
    wcfg.height = static_cast<int>(win_h);
    wcfg.min_width = 840;
    wcfg.min_height = 640;
    wcfg.borderless = false;
    wcfg.csd = true;

    auto win_res = backend_os::Window::create(*platform, wcfg);
    if (!win_res.isOk()) {
        std::cerr << "[-] Failed to create native OS window\n";
        return 1;
    }
    auto window = std::move(win_res.value());
    window->makeCurrent();

#ifdef NISABA_GLEW
    glewExperimental = GL_TRUE;
    glewInit();
    while (glGetError() != GL_NO_ERROR) {}
#endif

    auto device = GpuDevice::create();
    if (!device) {
        std::cerr << "[-] Failed to create GPU device\n";
        return 1;
    }

    const char* font_candidates[] = {
        "/usr/share/obsidian_shell/fonts/Inter-Regular.ttf",
        "/usr/local/share/obsidian_shell/fonts/Inter-Regular.ttf",
        "fonts/Inter-Regular.ttf",
        "../fonts/Inter-Regular.ttf",
        "/usr/share/fonts/truetype/inter/Inter-Regular.ttf",
        "/usr/share/fonts/TTF/Inter-Regular.ttf"
    };

    auto register_font = [&](GpuCanvas& c) {
        for (const char* fpath : font_candidates) {
            if (c.create_font("sans", fpath) >= 0) return true;
        }
        if (c.create_font_mem("sans",
                              sysmon::EmbeddedFont::inter_regular_data(),
                              sysmon::EmbeddedFont::inter_regular_size()) >= 0) {
            return true;
        }
        return false;
    };

    auto sz = window->getDrawableSize();
    if (sz.width > 0 && sz.height > 0) {
        win_w = static_cast<uint32_t>(sz.width);
        win_h = static_cast<uint32_t>(sz.height);
    }

    auto win_surface = GpuSurface::from_screen(device, win_w, win_h, 0);
    auto win_canvas = std::make_unique<GpuCanvas>(win_surface);
    register_font(*win_canvas);

    sysmon::Monitor monitor(sysmon::Preferences::defaultPath());
    sysmon::SystemMonitorApp app(monitor, static_cast<float>(win_w), static_cast<float>(win_h));
    app.set_window_and_platform(window.get(), platform.get());
    if (dark_mode) {
        app.set_dark_mode(true);
    }

    bool running = true;
    window->onClose().connect([&]() {
        running = false;
    });

    auto apply_resize = [&](uint32_t nw, uint32_t nh) {
        if (nw == 0 || nh == 0) return;
        if (nw == win_w && nh == win_h) return;
        win_w = nw;
        win_h = nh;
        win_canvas->resize(win_w, win_h);
        app.resize(static_cast<float>(win_w), static_cast<float>(win_h));
    };

    window->onResize().connect([&](int w, int h) {
        auto dsz = window->getDrawableSize();
        uint32_t nw = static_cast<uint32_t>(dsz.width > 0 ? dsz.width : w);
        uint32_t nh = static_cast<uint32_t>(dsz.height > 0 ? dsz.height : h);
        apply_resize(nw, nh);
    });

    float cur_mouse_x = 0.0f;
    float cur_mouse_y = 0.0f;

    platform->onMouseMove().connect([&](float x, float y) {
        cur_mouse_x = x;
        cur_mouse_y = y;
        app.handle_mouse_move(cur_mouse_x, cur_mouse_y);
    });

    platform->onMouseDown().connect([&](float x, float y, int button) {
        cur_mouse_x = x;
        cur_mouse_y = y;
        app.handle_mouse_down(cur_mouse_x, cur_mouse_y, button);
    });

    platform->onMouseUp().connect([&](float x, float y, int button) {
        cur_mouse_x = x;
        cur_mouse_y = y;
        app.handle_mouse_up(cur_mouse_x, cur_mouse_y, button);
    });

    platform->onScroll().connect([&](float /*dx*/, float dy) {
        app.handle_scroll(cur_mouse_x, cur_mouse_y, dy);
    });

    platform->onKeyDown().connect([&](int key, int mods) {
        app.handle_key_down(key, mods);
        if (app.should_quit()) {
            running = false;
        }
    });

    platform->onTextInput().connect([&](std::string_view text) {
        app.handle_text_input(text);
    });

    auto prev_time = std::chrono::steady_clock::now();

    while (running) {
        if (!platform->pollEvents()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - prev_time).count();
        prev_time = now;
        if (dt > 0.1f) dt = 0.1f;

        auto dsz = window->getDrawableSize();
        if (dsz.width > 0 && dsz.height > 0) {
            apply_resize(static_cast<uint32_t>(dsz.width), static_cast<uint32_t>(dsz.height));
        }

        app.update(dt);
        if (app.should_quit()) {
            break;
        }

        window->makeCurrent();
        win_canvas->clear(Color::from_rgba8(0, 0, 0, 0));
        app.render(*win_canvas);
        win_canvas->flush();
        window->swapBuffers();
    }
#endif

    std::cout << "[*] Obsidian System Monitor exited cleanly.\n";
    return 0;
}
