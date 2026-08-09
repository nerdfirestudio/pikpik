#!/bin/zsh
# Replay a recorded session headlessly.
#
#   tools/input_replay.sh <name> [build-dir] [extra env...]
#
# Restores the save snapshot taken at record time into a scratch card dir, so
# the initial conditions match. Verified: with the snapshot restored, a replay
# reproduces the recorded run BIT-IDENTICALLY (0 differing pixels); without it,
# ~85% of pixels differ and the recording's save fingerprint says why.
#
# Examples:
#   tools/input_replay.sh day1                       # -O0, reproduce
#   tools/input_replay.sh day1 build-o2              # the -O2 candidate
#   tools/input_replay.sh day1 build-ubsan-o2        # -O2 + UBSan
#   tools/input_replay.sh day1 build-o2 PC_DUMP_FRAMES=1200,1600
set -u
ROOT="${0:A:h:h}"
name="${1:-}"
build="${2:-build-m1}"
shift 2 2>/dev/null || shift $#
if [[ -z "$name" ]]; then
    print -u2 "usage: tools/input_replay.sh <name> [build-dir] [VAR=value ...]"
    exit 2
fi

rec="$ROOT/recordings/$name.rec"
savesnap="$ROOT/recordings/$name.save"
bin="$ROOT/$build/Pikmin"
[[ -f "$rec" ]] || { print -u2 "no recording at $rec"; exit 1; }
[[ -x "$bin" ]] || { print -u2 "no binary at $bin"; exit 1; }

card="${TMPDIR:-/tmp}/pikmin-replay-$name-card"
rm -rf "$card"
mkdir -p "$card"
[[ -d "$savesnap" ]] && cp -R "$savesnap/." "$card/" 2>/dev/null

cd "$ROOT/$build" || exit 1
exec env PC_INPUT_PLAY="$rec" PC_CARD_DIR="$card" "$@" ./Pikmin
