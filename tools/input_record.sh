#!/bin/zsh
# Record a play session for later headless replay.
#
#   tools/input_record.sh <name> [build-dir]
#
# Play normally; the recording is written as you go (flushed on every input
# change, so it survives a crash — which is usually the point). Quit with
# Cmd-Q when you are done.
#
# Records to recordings/<name>.rec, and snapshots the CURRENT save alongside it
# as recordings/<name>.save. Replay restores that snapshot, because the save
# state is part of the initial conditions: replaying against a different save
# desyncs, and the recording carries a fingerprint so it says so.
#
# Record on a build that does NOT crash (build-m1, -O0) and replay on the one
# that does. The port layer is compiled identically in every build, so a
# recording made at -O0 replays at -O2 unchanged.
set -u
ROOT="${0:A:h:h}"
name="${1:-}"
build="${2:-build-m1}"
if [[ -z "$name" ]]; then
    print -u2 "usage: tools/input_record.sh <name> [build-dir]"
    exit 2
fi

bin="$ROOT/$build/Pikmin"
[[ -x "$bin" ]] || { print -u2 "no binary at $bin — build it first (cd $build && make Pikmin)"; exit 1; }

outdir="$ROOT/recordings"
mkdir -p "$outdir"
rec="$outdir/$name.rec"
savesnap="$outdir/$name.save"

# Never clobber. Recording is expensive — it costs a human a play session —
# and re-running with the same name used to silently overwrite one (it ate a
# 15-minute day 1). Archive the old pair with its own timestamp instead.
if [[ -f "$rec" ]]; then
    stamp=$(date +%Y%m%d-%H%M%S)
    mv "$rec" "$outdir/$name-$stamp.rec"
    [[ -d "$savesnap" ]] && mv "$savesnap" "$outdir/$name-$stamp.save"
    print "existing recording archived as $name-$stamp.rec"
fi

# The save the session starts from. Default to the build's own save dir, which
# is where a loose build keeps it (docs/CONFIG.md).
card="${PC_CARD_DIR:-$ROOT/$build/save}"
rm -rf "$savesnap"
mkdir -p "$savesnap"
if [[ -d "$card" ]]; then
    cp -R "$card/." "$savesnap/" 2>/dev/null
fi
nsaved=$(find "$savesnap" -type f 2>/dev/null | wc -l | tr -d " ")
if [[ "$nsaved" == "0" ]]; then
    print "note: starting from NO save ($card is empty or absent)."
    print "      That is fine — replay will restore the same empty state — but"
    print "      the run will begin at the file-select screen, not a load."
fi

print "recording  -> $rec"
print "save snap  -> $savesnap  (from $card)"
print "build      -> $build"
print ""
print "Play. Quit with Cmd-Q when done."
print ""

cd "$ROOT/$build" || exit 1
PC_INPUT_REC="$rec" PC_CARD_DIR="$card" ./Pikmin

n=$(grep -vc '^#\|^save ' "$rec" 2>/dev/null || print 0)
print ""
print "recorded $n input changes over $(tail -1 "$rec" 2>/dev/null | cut -d' ' -f1) ticks -> $rec"
print "replay it with:  tools/input_replay.sh $name [build-dir]"
