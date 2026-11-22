#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# Obsidian System Monitor — VAXP-OS Debian (.deb) Packaging Script (Nisaba Engine)
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PKG_NAME="system-monitor"
PKG_VERSION="0.1.0"
PKG_ARCH="$(dpkg --print-architecture 2>/dev/null || echo "amd64")"
BUILD_DIR="$SCRIPT_DIR/build"
STAGE_DIR="$SCRIPT_DIR/pkg_stage/${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH}"
DEB_OUT="$SCRIPT_DIR/${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH}.deb"

echo "========================================================"
echo "  Packaging Obsidian System Monitor v${PKG_VERSION} (${PKG_ARCH})"
echo "========================================================"

# 1. Configure & Compile via Meson / Ninja
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    echo "[*] Configuring Meson build directory (prefix=/usr, buildtype=release)..."
    meson setup "$BUILD_DIR" --prefix=/usr --buildtype=release
else
    meson configure "$BUILD_DIR" --prefix=/usr
fi

echo "[*] Compiling target binary..."
meson compile -C "$BUILD_DIR"

# 2. Prepare clean Debian package staging root
echo "[*] Preparing staging tree at: $STAGE_DIR"
rm -rf "$SCRIPT_DIR/pkg_stage"
mkdir -p "$STAGE_DIR/DEBIAN"

# 3. Install Meson targets into staging dir
DESTDIR="$STAGE_DIR" meson install -C "$BUILD_DIR" --no-rebuild

# Strip binary symbols to reduce package footprint
if command -v strip >/dev/null 2>&1; then
    for bin in "$STAGE_DIR/usr/bin/system-monitor"; do
        if [ -f "$bin" ]; then
            echo "[*] Stripping debug symbols from $(basename "$bin")..."
            strip --strip-unneeded "$bin"
        fi
    done
fi

INSTALLED_SIZE="$(du -sk "$STAGE_DIR/usr" | cut -f1)"

# 4. Generate DEBIAN/control metadata
cat > "$STAGE_DIR/DEBIAN/control" <<EOF
Package: ${PKG_NAME}
Version: ${PKG_VERSION}
Section: utils
Priority: optional
Architecture: ${PKG_ARCH}
Installed-Size: ${INSTALLED_SIZE}
Depends: libc6, libstdc++6, libgcc-s1, libgl1, libegl1, libglew2.2, libvulkan1, libgbm1, libdrm2, libwayland-client0, libwayland-egl1, libwayland-cursor0, libxkbcommon0, libx11-6, libxext6, libxrandr2, libinput10, libudev1
Maintainer: VAXP-OS Developer <developer@vaxp.org>
Homepage: https://www.vaxp.org/
Description: Obsidian System Monitor for VAXP-OS
 Native C++20 GPU-accelerated system resource and process monitor
 built directly on the Nisaba Engine for VAXP-OS (Obsidian Shell & Asgard).
EOF

# 5. Build the .deb archive
echo "[*] Building Debian package..."
dpkg-deb --root-owner-group --build "$STAGE_DIR" "$DEB_OUT"

rm -rf "$SCRIPT_DIR/pkg_stage"
echo "[+] Package created: $DEB_OUT"

