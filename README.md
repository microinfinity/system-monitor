# System Monitor

A hardware-accelerated Linux system telemetry and process management application built in **C++20** on top of the **Nisaba** Graphics & Native OS Backend Engine (`nisaba` / `nisaba_backend_os`) for the **Obsidian** desktop environment and **VAXP-OS**.

---

## Technical Architecture

### 1. Rendering & UI Pipeline (`Nisaba GPU Engine`)
- **Direct GPU Canvas Rendering**: The entire user interface is rendered via `nisaba::gpu::GpuCanvas`, `nisaba::gpu::GpuSurface`, and `nisaba::gpu::GpuDevice` over OpenGL/Wayland with zero external widget-toolkit overhead.
- **Client-Side Decorations (`CsdFrame`)**: Custom hardware-rendered window chrome featuring per-pixel signed-distance-field (SDF) bevels, specular edge highlights, 8-edge interactive window resizing, titlebar drag/maximize gestures, and right-aligned window controls (`Close`, `Maximize`, `Minimize`) paired with a top-left theme polarity switch.
- **Vector & Icon Pipeline (`IconService` & `VectorIcons`)**:
  - Native SVG parsing, theme color injection (`currentColor`), and GPU texture baking via `nisaba::svg::SvgDocument` and `nisaba::svg::SvgCache`.
  - Multi-format application icon resolution supporting scalable `.svg`, `.png`, `.webp`, and XML-SVG payloads across XDG icon hierarchies (`WhiteSur`, `Nordzy`, `hicolor`, Flatpak, and portable application bundles).
- **Real-Time Spline Telemetry Graphs (`HistoryGraphRenderer`)**:
  - Monotone cubic Hermite / Bézier spline interpolation over a 60-second rolling time window.
  - Multi-series alpha-gradient area fills and anti-aliased stroke paths rendered directly on the GPU.
- **Physics-Driven Micro-Interactions**:
  - Critically damped spring solver ($\omega = 2\pi / T$, velocity-proportional stretch) for the scope segmented control (`Applications`, `My Processes`, `All Processes`).
  - Smooth inertial viewport scrolling with auto-fading overlay scrollbars.
- **Standalone Dual-Polarity Theme Engine (`theme/theme.hpp`)**:
  - Self-contained `sysmon::AppTheme` supporting real-time linear color interpolation (`AppTheme::lerp`) between **Light Mode** and **Dark Mode**, plus responsive layout scaling via `sysmon::UnitScale`.

### 2. Kernel Telemetry & Process Engine (`sysmon::Monitor` / `TelemetryCollector`)
- **Asynchronous Non-Blocking Sampling**:
  - Periodic background collection (`1000 ms`, `2000 ms`, or `5000 ms`) dispatched via worker futures so kernel I/O and filesystem queries never block the 60+ FPS render loop.
  - Isolated asynchronous volume probing (`statx` mount ID verification + `statvfs`) to prevent stalled network mounts from delaying process or hardware telemetry.
- **CPU & Memory Telemetry**:
  - Per-core and aggregate tick delta computation from `/proc/stat`.
  - Physical RAM (`MemTotal`, `MemAvailable`, `Cached`, `SReclaimable`) and Swap metrics from `/proc/meminfo`.
  - Per-process Proportional Set Size (`PSS`) sampling via `/proc/[pid]/smaps_rollup` with throttled page-table walks, falling back to Resident Set Size (`RSS`).
- **Storage & Network I/O**:
  - Physical block device throughput (`read` / `write` byte rates) and capacity discovery via `/proc/diskstats` and `/sys/block/*/device`.
  - Per-interface network transfer rates (`RX` / `TX`), operational state, and MAC address inspection via `/proc/net/dev` and `/sys/class/net/*`.
- **GPU & DRM Subsystem Telemetry**:
  - Automatic discovery of DRM cards (`/sys/class/drm/card*`), PCI vendor/device identification, `hwmon` thermal and clock sensors (`temp1_input`, `freq1_input`), and VRAM counters (`mem_info_vram_used`, `mem_info_vram_total`).
  - Per-client DRM engine utilization (`drm-engine-*`, `drm-cycles-*`, `drm-total-cycles-*`) aggregated across `/proc/[pid]/fdinfo/*` without waking runtime-suspended GPUs.
- **Desktop Application Grouping & Process Control**:
  - Heuristic `.desktop` catalog matching with canonical symlink resolution (`std::filesystem::canonical`), `StartupWMClass` correlation, `systemd`/`cgroup` unit scope extraction (`/proc/[pid]/cgroup`), Chromium/Electron helper role classification, and portable bundle (`product.json`) inspection.
  - Race-free process signaling (`SIGTERM` / `SIGKILL`) verified against PID reuse via Linux `pidfd_open` and `pidfd_send_signal` syscalls alongside `/proc/[pid]/stat` `startTicks` validation.

---

## Project Layout

```text
system-monitor/
├── include/
│   ├── csd_frame.hpp              # Client-side window frame & SDF control plates
│   ├── embedded_font.hpp          # Fallback embedded Inter font binary interface
│   ├── history_graph_renderer.hpp # GPU cubic spline & gradient area chart renderer
│   ├── icon_service.hpp           # XDG & bundle icon resolver + Nisaba SVG/raster cache
│   ├── system_monitor_app.hpp     # Main application state, views, modals & input router
│   ├── system_telemetry.hpp       # Linux kernel telemetry & process table data models
│   └── vector_icons.hpp           # Native Nisaba SVG UI icon renderer & badge generator
├── src/
│   ├── csd_frame.cpp              # Window frame rendering & Wayland/OS window actions
│   ├── embedded_font.cpp          # Embedded font payload
│   ├── main.cpp                   # Nisaba platform, Wayland window & GPU render loop
│   ├── system_monitor_app.cpp     # Sidebar, process table, resource pages & dialogs
│   └── system_telemetry.cpp       # /proc, /sys, DRM, statvfs & pidfd implementation
├── theme/
│   └── theme.hpp                  # Standalone Light/Dark theme palette & UnitScale
├── icons/                         # Vector SVG icon assets
├── meson.build                    # Meson build configuration
├── package.sh                     # Packaging helper script
└── system-monitor.desktop         # XDG desktop entry
```

---

## Build & Installation

### Dependencies
- C++20 compiler (`GCC >= 12` or `Clang >= 15`)
- `meson` and `ninja`
- `nisaba` and `nisaba_backend_os`
- `wayland-client`, `OpenGL` (`gl`), `GLEW`, `threads`

### Compile
```bash
meson setup build --prefix=/usr --buildtype=release
meson compile -C build
```

### Install
```bash
sudo meson install -C build
```

### Run
```bash
# Launch in Light Mode (default)
./build/system-monitor

# Launch in Dark Mode
./build/system-monitor --dark
```

### Keyboard Shortcuts
| Shortcut | Action |
| :--- | :--- |
| `Ctrl+F` | Focus process search bar |
| `Ctrl+D` / `Ctrl+T` | Toggle between Light Mode and Dark Mode |
| `Up` / `Down` | Navigate process table rows |
| `Enter` | Open Process / Application Details modal |
| `Delete` | Request Quit confirmation for selected target |
| `Escape` | Close active modal, context menu, or clear search filter |
| `Ctrl+Q` | Exit System Monitor |

---

This project was developed by the MicroInfinity organization team as a core unit integrated within the VAXP-OS operating system.

