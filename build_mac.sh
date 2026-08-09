#!/bin/bash
# build_mac.sh — configure and build the macOS binary.
#
#   ./build_mac.sh            # build into build-m1 (the usual dev tree)
#   ./build_mac.sh -c         # wipe the build directory first
#   BUILD_DIR=build-rel ./build_mac.sh
#
# This is deliberately thin: the interesting configuration lives in
# pc/CMakeLists.txt. It exists so that "how do I build this" has one answer.
set -euo pipefail

cd "$(dirname "$0")"

BUILD_DIR="${BUILD_DIR:-build-m1}"
# Keep in step with PC_APP_NAME in pc/CMakeLists.txt — this only names the
# binary the final message points at; CMake owns the actual target name.
APP_NAME="${APP_NAME:-Pikmin}"
CLEAN=0

# Optional per-tree overrides (APP_NAME, APP_ID, PC_CMAKE_ARGS). Absent in this
# repository; the published tree ships one to point the build at the sources
# setup.sh fetches and to build under its own name. Sourced after the cd above,
# so relative paths in it resolve against the repo root.
# REPO_DIR is what build.conf resolves its paths against — build_ios.sh does
# not cd, so $(pwd) is not a usable anchor there.
REPO_DIR="$(pwd)"
PC_CMAKE_ARGS="${PC_CMAKE_ARGS:-}"
# shellcheck disable=SC1091
[ -f build.conf ] && . ./build.conf


while [ $# -gt 0 ]; do
    case "$1" in
        -c|--clean) CLEAN=1 ;;
        -h|--help)  sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

if [ "$CLEAN" = 1 ]; then
    echo "==> removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake not found — brew install cmake" >&2
    exit 1
fi

# SDL2 comes from Homebrew. sdl2-compat is fine (it is what this tree is built
# against); it shims SDL2 onto SDL3 and dlopens SDL3 at runtime, which
# bundle_mac.sh has to account for.
if [ ! -d /opt/homebrew/include/SDL2 ]; then
    echo "SDL2 headers not found in /opt/homebrew/include/SDL2 — brew install sdl2" >&2
    exit 1
fi

echo "==> configuring $BUILD_DIR"
cmake -S pc -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug \
    -DPC_APP_NAME="$APP_NAME" $PC_CMAKE_ARGS "$@" >/dev/null

echo "==> building"
cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)"

echo
echo "built: $BUILD_DIR/$APP_NAME"
if [ ! -e "$BUILD_DIR/gamedata" ]; then
    echo
    echo "NOTE: $BUILD_DIR/gamedata does not exist. The game needs an extracted"
    echo "      GPIE01_01 disc filesystem there (a symlink is fine) — it is the"
    echo "      directory CONTAINING dataDir. Or set PC_GAME_ROOT."
fi
