#!/bin/bash
# setup.sh — fetch the Pikmin decompilation and apply this port's patches.
#
#   ./setup.sh              # fetch into vendor/pikmin and patch it
#   ./setup.sh --check      # verify an existing vendor/pikmin, change nothing
#   ./setup.sh --force      # discard local changes in vendor/pikmin and redo
#
# WHY THIS EXISTS
#
# This repository contains the port layer and its patches, and no game code.
# The decompiled sources belong to a separate upstream project and are fetched
# from it here, at a pinned commit, rather than copied into this repository.
# docs/LEGAL.md section 4 explains the reasoning.
#
# What you end up with:
#
#   vendor/pikmin/          upstream decompilation at DECOMP_COMMIT
#     src/ include/         ... with patches/*.patch applied
#                           ... plus core-additions/ copied in
#
# vendor/ is gitignored. Nothing this script produces should ever be committed
# to this repository.
set -euo pipefail

cd "$(dirname "$0")"
ROOT="$(pwd)"

# --- The pin -----------------------------------------------------------------
# A full 40-character SHA, not a branch or tag: it is the exact tree the
# patches were generated against and the only one they are known to apply to.
# A tag can be moved; a branch certainly will be.
DECOMP_REPO="${DECOMP_REPO:-https://github.com/projectPiki/pikmin.git}"
DECOMP_COMMIT="${DECOMP_COMMIT:-f193f419501bdba101b9b3672dec4953d36af945}"
VENDOR="${VENDOR:-vendor/pikmin}"

MODE=apply
while [ $# -gt 0 ]; do
    case "$1" in
        --check) MODE=check ;;
        --force) MODE=force ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

command -v git >/dev/null || { echo "git not found" >&2; exit 1; }

say()  { printf '==> %s\n' "$*"; }
fail() { printf 'setup.sh: %s\n' "$*" >&2; exit 1; }

# --- Fetch -------------------------------------------------------------------
# Fetch the one commit rather than cloning the history: it is a fraction of the
# transfer, and pinning by SHA means there is nothing else we would ever want.
fetch_decomp() {
    say "fetching decompilation at ${DECOMP_COMMIT:0:12}"
    mkdir -p "$(dirname "$VENDOR")"
    if [ ! -d "$VENDOR/.git" ]; then
        rm -rf "$VENDOR"
        git init -q "$VENDOR"
        git -C "$VENDOR" remote add origin "$DECOMP_REPO"
    fi
    if ! git -C "$VENDOR" cat-file -e "$DECOMP_COMMIT^{commit}" 2>/dev/null; then
        git -C "$VENDOR" fetch -q --depth 1 origin "$DECOMP_COMMIT" \
            || fail "could not fetch $DECOMP_COMMIT from $DECOMP_REPO
    The upstream project may have rewritten history, or you may be offline.
    Nothing else in this repository can substitute for it."
    fi
    git -C "$VENDOR" -c advice.detachedHead=false checkout -q --force "$DECOMP_COMMIT"
    git -C "$VENDOR" clean -qfd src include 2>/dev/null || true

    # Prove we got the tree we pinned, not merely a commit by that name.
    local got; got="$(git -C "$VENDOR" rev-parse HEAD)"
    [ "$got" = "$DECOMP_COMMIT" ] || fail "checked out $got, expected $DECOMP_COMMIT"
    say "upstream at $got"
}

# --- Patch -------------------------------------------------------------------
# Patches are numbered and applied in order. They touch disjoint paths, so the
# order is for readability rather than correctness — but --check first means a
# failure leaves the tree untouched instead of half-patched.
apply_patches() {
    local p
    for p in patches/*.patch; do
        [ -e "$p" ] || fail "no patches found in patches/"
        git -C "$VENDOR" apply --check "$ROOT/$p" 2>/dev/null \
            || fail "$p does not apply cleanly to $DECOMP_COMMIT
    The vendored tree may be dirty — try ./setup.sh --force.
    If it is clean, the patches and the pin have drifted apart; regenerate
    them with tools/regen_patches.sh."
        git -C "$VENDOR" apply "$ROOT/$p"
        say "applied $(basename "$p")"
    done
}

# Source files this port ADDS to the game tree. They are entirely our own work,
# so they live in the repository as ordinary readable source rather than being
# smuggled in as new-file diffs. Copied, not symlinked: the build must not be
# able to write back into this repository.
copy_additions() {
    [ -d core-additions ] || return 0
    say "copying core-additions/"
    (cd core-additions && find . -type f ! -name '.*') | while read -r rel; do
        mkdir -p "$VENDOR/$(dirname "$rel")"
        cp "core-additions/$rel" "$VENDOR/$rel"
        printf '    %s\n' "${rel#./}"
    done
}

# --- Verify ------------------------------------------------------------------
verify() {
    local missing=0 f
    for f in src/sysBootup.cpp include/types.h src/jaudio/bx_pc.c include/pc_uninit.h; do
        [ -f "$VENDOR/$f" ] || { echo "    MISSING $f" >&2; missing=1; }
    done
    [ "$missing" = 0 ] || fail "the patched tree is incomplete"
    say "verified: $VENDOR looks complete"
}

# --- Run ---------------------------------------------------------------------
case "$MODE" in
    check)
        [ -d "$VENDOR/.git" ] || fail "$VENDOR does not exist — run ./setup.sh"
        got="$(git -C "$VENDOR" rev-parse HEAD 2>/dev/null || echo none)"
        [ "$got" = "$DECOMP_COMMIT" ] \
            || fail "$VENDOR is at $got, expected $DECOMP_COMMIT — run ./setup.sh --force"
        verify
        exit 0
        ;;
    force)
        rm -rf "$VENDOR"
        ;;
esac

if [ -d "$VENDOR/.git" ] && ! git -C "$VENDOR" diff --quiet 2>/dev/null; then
    say "$VENDOR already patched — resetting it before reapplying"
fi

fetch_decomp
apply_patches
copy_additions
verify

cat <<EOF

Setup complete.

  decompilation   $VENDOR (upstream ${DECOMP_COMMIT:0:12}, patched)
  next            ./build_mac.sh   then ./bundle_mac.sh      (macOS)
                  ./build_ios.sh   then open app/pikpik.xcodeproj  (iOS)

You still need game data. See docs/GAME_DATA.md — it is not included and
cannot be, and the build does nothing useful without it.
EOF
