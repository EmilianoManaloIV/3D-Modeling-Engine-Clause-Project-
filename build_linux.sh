#!/usr/bin/env bash
# Builds Modeler3D for Linux and copies the executable to dist/linux/.
# Needs: a C++17 compiler, CMake 3.16+, and the X11 + OpenGL headers:
#   Debian/Ubuntu:  sudo apt install build-essential cmake libx11-dev libgl-dev
#   Fedora:         sudo dnf install gcc-c++ cmake libX11-devel mesa-libGL-devel
#   Arch:           sudo pacman -S base-devel cmake libx11 mesa
set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR="${BUILD_DIR:-build-linux}"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$BUILD_DIR" -j "$(nproc 2>/dev/null || echo 4)"

mkdir -p dist/linux
cp "$BUILD_DIR/Modeler3D" dist/linux/Modeler3D
chmod +x dist/linux/Modeler3D
echo
echo "Done: dist/linux/Modeler3D   (run it with ./dist/linux/Modeler3D)"
