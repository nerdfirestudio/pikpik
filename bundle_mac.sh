#!/bin/bash
# bundle_mac.sh — assemble the .app bundle from an existing build.
#
#   ./bundle_mac.sh              # game data referenced in place (fast; not movable)
#   ./bundle_mac.sh --copy-data  # game data copied in (~640 MB; app is self-contained)
#
# Produces dist/$APP_NAME.app, ad-hoc signed so Gatekeeper will run it locally.
# Ad-hoc signing is NOT notarisation: this is for running the thing yourself,
# not for handing to someone else.
#
# The data itself is never redistributable — it is the retail disc — so the
# default leaves it where it is and records the path in
# Contents/Resources/gamedata.path. --copy-data exists for making a
# self-contained app on your own machine.
set -euo pipefail

cd "$(dirname "$0")"

BUILD_DIR="${BUILD_DIR:-build-m1}"
DIST_DIR="${DIST_DIR:-dist}"
# The application's name and bundle id. Overridable because the published tree
# ships under a different name than this one; keep in step with PC_APP_NAME in
# pc/CMakeLists.txt, which names the executable this script looks for.
APP_NAME="${APP_NAME:-Pikmin}"
APP_ID="${APP_ID:-local.pikmin.metal}"
COPY_DATA=0

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

APP="$DIST_DIR/$APP_NAME.app"

while [ $# -gt 0 ]; do
    case "$1" in
        --copy-data) COPY_DATA=1 ;;
        -h|--help)   sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

BIN="$BUILD_DIR/$APP_NAME"
[ -x "$BIN" ] || { echo "no binary at $BIN — run ./build_mac.sh first" >&2; exit 1; }

DATA_SRC="$BUILD_DIR/gamedata"
[ -e "$DATA_SRC" ] || { echo "no game data at $DATA_SRC (see build_mac.sh)" >&2; exit 1; }
# Resolve through the symlink the dev tree normally uses.
DATA_REAL="$(cd "$DATA_SRC" && pwd -P)"

echo "==> laying out $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks"

cp "$BIN" "$APP/Contents/MacOS/$APP_NAME"

# ---- Info.plist ------------------------------------------------------------
# NSHighResolutionCapable matters: without it AppKit hands back a 1x backing
# store and the Metal layer renders soft on a Retina display.
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>              <string>$APP_NAME</string>
    <key>CFBundleDisplayName</key>       <string>$APP_NAME</string>
    <key>CFBundleExecutable</key>        <string>$APP_NAME</string>
    <key>CFBundleIconFile</key>          <string>$APP_NAME</string>
    <key>CFBundleIdentifier</key>        <string>$APP_ID</string>
    <key>CFBundlePackageType</key>       <string>APPL</string>
    <key>CFBundleInfoDictionaryVersion</key> <string>6.0</string>
    <key>CFBundleShortVersionString</key><string>0.1</string>
    <key>CFBundleVersion</key>           <string>0.1</string>
    <key>LSMinimumSystemVersion</key>    <string>12.0</string>
    <key>NSHighResolutionCapable</key>   <true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key> <true/>
    <key>LSApplicationCategoryType</key> <string>public.app-category.games</string>
</dict>
</plist>
PLIST

# ---- dependent dylibs ------------------------------------------------------
# Copy every non-system dylib the binary links, and repoint it at
# @executable_path/../Frameworks. One level is enough here (SDL2 pulls in
# nothing but system frameworks), but the loop re-scans what it copies so a
# future dependency does not silently get missed.
copy_deps() {
    local target="$1"
    otool -L "$target" | tail -n +2 | awk '{print $1}' | while read -r dep; do
        case "$dep" in
            /usr/lib/*|/System/*|@*) continue ;;
        esac
        local base; base="$(basename "$dep")"
        if [ ! -f "$APP/Contents/Frameworks/$base" ]; then
            echo "    bundling $base"
            cp "$dep" "$APP/Contents/Frameworks/$base"
            chmod u+w "$APP/Contents/Frameworks/$base"
            install_name_tool -id "@executable_path/../Frameworks/$base" \
                "$APP/Contents/Frameworks/$base" 2>/dev/null || true
            copy_deps "$APP/Contents/Frameworks/$base"
        fi
        install_name_tool -change "$dep" "@executable_path/../Frameworks/$base" "$target" 2>/dev/null || true
    done
}
echo "==> bundling dylibs"
copy_deps "$APP/Contents/MacOS/$APP_NAME"

# sdl2-compat is a shim that dlopens SDL3 BY NAME at runtime — it does not link
# it, so the otool walk above cannot see it. Copy it in and add an rpath so the
# dlopen resolves inside the bundle instead of against Homebrew.
if otool -L "$APP/Contents/MacOS/$APP_NAME" 2>/dev/null | grep -q SDL2 || \
   ls "$APP/Contents/Frameworks" 2>/dev/null | grep -q SDL2; then
    for sdl3 in /opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib /opt/homebrew/lib/libSDL3.0.dylib; do
        if [ -f "$sdl3" ] && [ ! -f "$APP/Contents/Frameworks/$(basename "$sdl3")" ]; then
            echo "    bundling $(basename "$sdl3") (dlopened by sdl2-compat)"
            cp "$sdl3" "$APP/Contents/Frameworks/"
            chmod u+w "$APP/Contents/Frameworks/$(basename "$sdl3")"
            break
        fi
    done
fi
install_name_tool -add_rpath "@executable_path/../Frameworks" \
    "$APP/Contents/MacOS/$APP_NAME" 2>/dev/null || true

# ---- icon ------------------------------------------------------------------
# macOS wants an .icns, which is a container of several square sizes at 1x and
# 2x. iconutil builds one from a directory of exactly-named PNGs; sips does the
# resampling. Done here rather than checked in so the source stays a single
# editable PNG.
# First existing candidate wins, so both this tree and the published one find
# their own icon without the script needing to know which it is in.
ICON_SRC="${ICON_SRC:-$(ls -1 assets/icon.png pikmin-icon.png 2>/dev/null | head -1)}"
if [ -n "$ICON_SRC" ] && [ -f "$ICON_SRC" ]; then
    echo "==> icon from $ICON_SRC"
    ICONSET="$(mktemp -d)/$APP_NAME.iconset"
    mkdir -p "$ICONSET"
    for sz in 16 32 128 256 512; do
        sips -z $sz $sz         "$ICON_SRC" --out "$ICONSET/icon_${sz}x${sz}.png"    >/dev/null 2>&1
        sips -z $((sz*2)) $((sz*2)) "$ICON_SRC" --out "$ICONSET/icon_${sz}x${sz}@2x.png" >/dev/null 2>&1
    done
    if iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/$APP_NAME.icns" 2>/dev/null; then
        echo "    $APP_NAME.icns"
    else
        echo "    iconutil failed — bundling without an icon" >&2
    fi
    rm -rf "$(dirname "$ICONSET")"
else
    echo "==> no icon source found — bundling without an icon"
fi

# ---- game data -------------------------------------------------------------
if [ "$COPY_DATA" = 1 ]; then
    echo "==> copying game data (this is ~640 MB)"
    cp -R "$DATA_REAL" "$APP/Contents/Resources/gamedata"
else
    # NOT a symlink: codesign rejects one whose destination escapes the bundle
    # ("invalid destination for symbolic link in bundle"), which would leave the
    # app unsignable. A plain pointer file is ordinary signable content.
    echo "==> pointing at game data in place (pass --copy-data for a self-contained app)"
    printf '%s\n' "$DATA_REAL" > "$APP/Contents/Resources/gamedata.path"
fi

# ---- sign ------------------------------------------------------------------
# Ad-hoc (-s -) and deep, after everything is in place: signing before the last
# file is written invalidates the signature.
echo "==> ad-hoc signing"
codesign --force --deep --sign - "$APP" 2>&1 | sed 's/^/    /' || {
    echo "codesign failed — the app may still run, but Gatekeeper will complain" >&2
}

echo
echo "built: $APP"
echo "run:   open $APP        (or $APP/Contents/MacOS/$APP_NAME for a console)"
echo "user data: ~/Library/Application Support/$APP_NAME"
