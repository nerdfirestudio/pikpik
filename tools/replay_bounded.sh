#!/bin/zsh
# Replay a recording (or boot a scenario) headlessly for a bounded time, then
# stop it and summarise.
#
#   tools/replay_bounded.sh <label> <seconds> [VAR=value ...] -- [recording]
#
# Examples:
#   tools/replay_bounded.sh boss 300 PC_GEN_TRACE=1 -- day4
#   tools/replay_bounded.sh title 75 PC_AUTO_TITLE=1 PC_LOGO_HOLD=1
#
# Why this exists rather than `timeout`:
#   * `timeout` is not on macOS. `timeout 300 ./Pikmin` exits 127 having run
#     NOTHING, which looks exactly like a clean pass.
#   * A replay whose input runs out PARKS instead of exiting, so every replay
#     needs an explicit bound or it runs forever.
#   * It SIGTERMs first so the uninit probe can print its per-site totals
#     (a SIGKILL skips atexit and loses them), and only SIGKILLs if that fails.
#   * It kills ONLY the pid it launched. Never `pkill -9 -x Pikmin` — that
#     killed the user's own session once.
#
# THIS IS A CRASH/ASSERT GATE, NOT A RENDER MEASUREMENT. It launches the game
# detached from a non-interactive shell, and a replay run that way draws
# measurably LESS than the same recording run in the foreground: day4-2 peaks at
# 215805 verts here and 265761 under `tools/input_replay.sh` in a terminal — the
# same recording, same save fingerprint, same binary, three runs each, one of
# them to the recording's end (v95). The cause is not diagnosed.
#
# So never size a cap, quote a peak, or judge a rendering change from this
# script's output. Use tools/input_replay.sh in the foreground for anything that
# measures how much the frame drew.
#
# Logs land in scratch/replay-logs/<label>.log.
set -u
ROOT="${0:A:h:h}"
OUT="$ROOT/scratch/replay-logs"
mkdir -p "$OUT"

label="$1"; shift
secs="$1"; shift

envs=()
replay=""
while (( $# )); do
    if [[ "$1" == "--" ]]; then shift; replay="${1:-}"; shift 2>/dev/null || true; continue; fi
    envs+=("$1"); shift
done

log="$OUT/$label.log"
: > "$log"

# ALWAYS run on a scratch card, replay or not. This used to be inside the
# `if replay` below, so a gate with no recording — a title gate, a movie gate,
# PC_CARD_SELFTEST — fell through to the default card dir, which is the USER'S
# LIVE SAVE. A gate must never be able to touch the save the user is playing:
# it is the one piece of state in this repo that cannot be rebuilt from source,
# and the selftest's whole job is creating and deleting files on a card.
card="${TMPDIR:-/tmp}/pikmin-gate-$label-card"
rm -rf "$card"; mkdir -p "$card"
envs+=("PC_CARD_DIR=$card")

if [[ -n "$replay" ]]; then
    rec="$ROOT/recordings/$replay.rec"
    snap="$ROOT/recordings/$replay.save"
    # Seed from the recording's snapshot: without it the replay desyncs (~85% of
    # pixels) and the fingerprint line says why.
    [[ -d "$snap" ]] && cp -R "$snap/." "$card/" 2>/dev/null
    envs+=("PC_INPUT_PLAY=$rec")
fi

cd "$ROOT/build-m1" || exit 1
env PC_UNINIT_PROBE=1 "${envs[@]}" "$ROOT/build-m1/Pikmin" >>"$log" 2>&1 &
pid=$!
print "[replay] $label pid=$pid timeout=${secs}s env=${envs[*]}"

# Poll so a gate that exits on its own finishes early instead of burning the
# whole budget.
i=0
while (( i < secs )); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 1
    (( i++ ))
done

if kill -0 "$pid" 2>/dev/null; then
    # Still parked. SIGTERM so the probe's handler can print the per-site totals;
    # SIGKILL only if it does not go down.
    kill -TERM "$pid" 2>/dev/null
    j=0
    while (( j < 8 )); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 1; (( j++ ))
    done
    kill -9 "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    print "[replay] $label terminated after ${secs}s (parked)"
else
    wait "$pid" 2>/dev/null
    rc=$?
    print "[replay] $label exited rc=$rc after ${i}s"
fi

# A gate opens a real window, and closing it is SDL_QUIT -> exit(0). That is
# indistinguishable from a clean finish by exit code alone: a gate the user shut
# after 9 seconds of a 30586-tick recording reported "exited rc=0" and looked
# like a pass. Say so instead, and say how far the replay actually got — this
# repo has been burned by a "passing" run that executed nothing (the macOS
# `timeout` trap).
# Report coverage and abort SEPARATELY, not as alternatives. A window closed
# AFTER the recording ran out is a complete gate; closed before it is not, and
# only the coverage line tells them apart. The first version of this check made
# them exclusive and reported a fully-covered day4-2 run as "NOT a pass".
if [[ -n "$replay" ]]; then
    if grep -q "recording exhausted" "$log"; then
        print "[replay] $label reached the end of the recording"
    else
        print "[replay] $label did NOT reach the end of the recording — partial coverage only"
    fi
fi
if grep -q "^\[PC\] quit requested" "$log"; then
    print "[replay] $label window was closed (SDL_QUIT) — exit code 0 does NOT mean pass here;"
    print "         trust the coverage line above, and re-run if it says partial."
fi

print "[replay] $label uninit lines:"
grep -E "^\[UNINIT\]" "$log" | sort -u | sed 's/^/    /'
grep -cE "^\[UNINIT\] FIRST" "$log" | sed 's/^/    distinct-first-hits: /'
