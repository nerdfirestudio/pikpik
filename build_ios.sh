#!/bin/bash
# build_ios.sh — Build the Pikmin engine for iOS (M-series iPad target).
#
# Produces:
#   ios/sdl2-ios/lib/libSDL2.a       static SDL2 for iOS (prebuilt/cached)
#   build-ios/lib/lib$APP_NAME.a     entry point (main -> SDL_main)
#   build-ios/lib/libpikmin_core.a   the decompiled game
#   build-ios/lib/libpikmin_pc.a     the port layer (Metal/SDL/audio/card)
#
# The Xcode shell ($XCODE_DIR/$APP_NAME.xcodeproj) links all four and owns signing,
# the Info.plist, and deployment. See docs/BUILDING_IOS.md.
#
# NOTE: make, not ninja — the ninja generator is a known trap in this tree.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDL_VER="2.32.10"
IOS_DEPLOY="16.4"   # >= 16.4 so M-series BC7 (HD texture packs) is available
SDL_SRC="$SCRIPT_DIR/ios/SDL2-$SDL_VER"
SDL_PREFIX="$SCRIPT_DIR/ios/sdl2-ios"
CORE_BUILD="$SCRIPT_DIR/build-ios"
# Keep in step with PC_APP_NAME in pc/CMakeLists.txt: it names both the CMake
# target built below and the lib$APP_NAME.a the Xcode shell links.
APP_NAME="${APP_NAME:-Pikmin}"
# Directory holding the Xcode shell project. Differs between this tree and the
# published one, so it is a variable rather than assumed to match APP_NAME.
XCODE_DIR="${XCODE_DIR:-$APP_NAME}"
# Optional per-tree overrides — see build_mac.sh.
# REPO_DIR is what build.conf resolves its paths against — build_ios.sh does
# not cd, so $(pwd) is not a usable anchor there.
REPO_DIR="$SCRIPT_DIR"
PC_CMAKE_ARGS="${PC_CMAKE_ARGS:-}"
# shellcheck disable=SC1091
[ -f "$SCRIPT_DIR/build.conf" ] && . "$SCRIPT_DIR/build.conf"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 8)"

# --- SDL2 (fetch + build only if the prebuilt prefix is missing) ---
if [ ! -f "$SDL_PREFIX/lib/libSDL2.a" ]; then
    if [ ! -d "$SDL_SRC" ]; then
        echo "=== Fetching SDL2 $SDL_VER ==="
        mkdir -p "$SCRIPT_DIR/ios"
        curl -sL -o "$SCRIPT_DIR/ios/SDL2.tar.gz" \
            "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz"
        tar xzf "$SCRIPT_DIR/ios/SDL2.tar.gz" -C "$SCRIPT_DIR/ios"
        rm "$SCRIPT_DIR/ios/SDL2.tar.gz"
    fi
    echo "=== Building SDL2 static for iOS ==="
    cmake -S "$SDL_SRC" -B "$SCRIPT_DIR/ios/build-sdl2" \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_DEPLOY" \
        -DCMAKE_BUILD_TYPE=Release \
        -DSDL_STATIC=ON -DSDL_SHARED=OFF -DSDL_TEST=OFF \
        -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX"
    cmake --build "$SCRIPT_DIR/ios/build-sdl2" -j"$JOBS"
    cmake --install "$SCRIPT_DIR/ios/build-sdl2"
fi

# --- Engine static libs ---
echo "=== Configuring pikmin (iOS) ==="
cmake -S "$SCRIPT_DIR/pc" -B "$CORE_BUILD" \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_DEPLOY" \
    -DPC_APP_NAME="$APP_NAME" $PC_CMAKE_ARGS \
    "$@"

echo "=== Building (-j$JOBS) ==="
cmake --build "$CORE_BUILD" --target "$APP_NAME" -j"$JOBS"

echo ""
echo "=== iOS engine build complete ==="
ls -la "$CORE_BUILD/lib/"
echo "SDL2:  $SDL_PREFIX/lib/libSDL2.a"
echo "Next:  open $XCODE_DIR/$APP_NAME.xcodeproj and Run to the device."
