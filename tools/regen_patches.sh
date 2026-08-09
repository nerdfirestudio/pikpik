#!/bin/bash
# regen_patches.sh — rewrite patches/ from your modified vendor/pikmin tree.
#
#   ./tools/regen_patches.sh            # regenerate patches/ in place
#   ./tools/regen_patches.sh --check    # fail if patches/ is out of date
#
# The workflow for changing the game code is:
#
#   1. edit files under vendor/pikmin/src or vendor/pikmin/include
#   2. build and test
#   3. run this script
#   4. commit the changed patches/ — never the vendored files themselves
#
# vendor/ is gitignored precisely so step 4 cannot go wrong by accident. See
# docs/LEGAL.md for why the game sources are not kept in this repository.
#
# A NEW file you add to the game tree does not belong in a patch. Put it in
# core-additions/, mirroring its path under the game tree, and setup.sh will
# copy it in. That keeps your own code readable as source instead of buried in
# a diff.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

VENDOR="${VENDOR:-vendor/pikmin}"
DECOMP_COMMIT="${DECOMP_COMMIT:-f193f419501bdba101b9b3672dec4953d36af945}"
CHECK=0
[ "${1:-}" = "--check" ] && CHECK=1

say()  { printf '==> %s\n' "$*"; }
fail() { printf 'regen_patches.sh: %s\n' "$*" >&2; exit 1; }

[ -d "$VENDOR/.git" ] || fail "$VENDOR does not exist — run ./setup.sh first"
[ "$(git -C "$VENDOR" rev-parse HEAD)" = "$DECOMP_COMMIT" ] \
    || fail "$VENDOR is not at the pinned commit — run ./setup.sh --force and redo your edits"

OUT="$ROOT/patches"
[ "$CHECK" = 1 ] && OUT="$(mktemp -d)/patches" && mkdir -p "$OUT"

# Stage everything, then unstage our own additions: they ship as real files in
# core-additions/, not as new-file diffs.
git -C "$VENDOR" add -A src include
ADDED="$(git -C "$VENDOR" diff --cached --name-only --diff-filter=A -- src include)"
if [ -n "$ADDED" ]; then
    # shellcheck disable=SC2086
    git -C "$VENDOR" rm -q --cached $ADDED
    for rel in $ADDED; do
        if [ ! -f "core-additions/$rel" ]; then
            fail "$rel is new but is not in core-additions/.
    Copy it there (keeping its path) so setup.sh installs it, then re-run."
        fi
    done
fi

DELETED="$(git -C "$VENDOR" diff --cached --name-only --diff-filter=D -- src include)"
[ -z "$DELETED" ] || fail "deleting upstream files is not supported by this flow:
$(printf '      %s\n' $DELETED)"

# Same groups the tree was published with. Disjoint by path; the numbering is
# for reading order, not for apply order.
group() {
    local name="$1"; shift
    git -C "$VENDOR" diff --cached -- "$@" > "$OUT/$name.patch"
    [ -s "$OUT/$name.patch" ] || rm -f "$OUT/$name.patch"
}

mkdir -p "$OUT"
rm -f "$OUT"/*.patch
group 0001-headers-host-types-and-lp64 include
group 0002-audio-and-video-decoders    src/jaudio src/hvqm4dec
group 0003-system-layer                src/sysCommon src/sysDolphin src/sysBootup.cpp
group 0004-game-code                   src/plugPikiColin src/plugPikiKando \
                                       src/plugPikiNakata src/plugPikiNishimura \
                                       src/plugPikiOgawa src/plugPikiYamashita

# Nothing may fall between the groups: a modified file that no group claims
# would vanish from the published tree without a word.
STAGED="$(git -C "$VENDOR" diff --cached --name-only -- src include | sort -u)"
COVERED="$(cat "$OUT"/*.patch 2>/dev/null | sed -n 's|^diff --git a/\(.*\) b/.*|\1|p' | sort -u)"
MISSED="$(comm -23 <(printf '%s\n' "$STAGED") <(printf '%s\n' "$COVERED"))"
[ -z "$MISSED" ] || fail "modified files no patch group claims — add them to a group above:
$(printf '      %s\n' $MISSED)"

if [ "$CHECK" = 1 ]; then
    if diff -rq "$ROOT/patches" "$OUT" >/dev/null 2>&1; then
        say "patches/ is up to date"
    else
        diff -rq "$ROOT/patches" "$OUT" || true
        fail "patches/ is out of date — run ./tools/regen_patches.sh"
    fi
    rm -rf "$(dirname "$OUT")"
    exit 0
fi

say "regenerated $(ls -1 "$OUT"/*.patch | wc -l | tr -d ' ') patches from $VENDOR"
for p in "$OUT"/*.patch; do
    printf '    %-44s %5s files\n' "$(basename "$p")" "$(grep -c '^diff --git' "$p")"
done
printf '\nReview with: git diff patches/\n'
