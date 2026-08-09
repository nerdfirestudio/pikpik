#!/bin/zsh
# Record a play session with the uninit probe armed, and say plainly whether
# anything NEW fired.
#
#   tools/probe_session.sh <name> [build-dir]     # play; Cmd-Q when done
#   tools/probe_session.sh --summary              # verdict across all sessions
#
# Why this exists: reaching a boss takes several days of play, so the evidence
# for "the 16 latent sites stay latent" arrives as a SEQUENCE of
# sessions rather than one run. Five sessions is five chances for the one line
# that matters to scroll past unnoticed, so this pins the log beside the
# recording and reduces each session to a one-line verdict.
#
# Chaining works naturally: PC_CARD_DIR defaults to <build>/save, which persists
# between runs, so day N starts from day N-1's save and input_record.sh
# snapshots that as day N's initial conditions.
#
# NOTE the probe measures PATH TAKEN, not uninitialized-ness — the three known
# sites still fire after being fixed, because the path is still walked and only
# the value read there is now defined. That is why this greps for UNKNOWN sites
# rather than for any site at all.
set -u
ROOT="${0:A:h:h}"

# Reached on every run since v91 and expected. cardutil.cpp:887 is deliberately
# NOT here: its fix removed the runtime path, so it firing again is a regression.
KNOWN=(
    "cmdqueue.c:159"
    "moviePlayer.cpp:234"
    "hvqm_play.c:723"
)

verdict() {
    local log="$1" label="$2"
    local -a new
    local line site
    # Every first hit prints as "[UNINIT] FIRST <file:line> <var> value=..."
    for line in ${(f)"$(grep -E '^\[UNINIT\] FIRST' "$log" 2>/dev/null)"}; do
        site="${${line#\[UNINIT\] FIRST }%% *}"
        [[ -z "$site" ]] && continue
        if [[ ${KNOWN[(Ie)$site]} -eq 0 ]]; then
            new+=("$site")
        fi
    done

    local crashes
    crashes=$(grep -icE "sigsegv|segmentation|\[CRASH\]|assertion" "$log" 2>/dev/null)

    # Every deliberate loud alarm in pc/ is written as "*** <what went wrong>" —
    # capacity overflows, dropped geometry, a wrong-texture merge. The verdict
    # used to report ONLY uninit sites and crashes, so a session that overflowed
    # a render cap still printed "no new uninit sites" and read as clean. That is
    # the silent-pass shape this project keeps paying for.
    #
    # Matching on "***" rather than a list means a loud warning added later joins
    # this verdict for free. Keep writing them that way.
    #
    # OVERFLOWING is in here too, and separately from "***", because a high-water
    # line can report a cap being exceeded in a session whose loud warning never
    # fired — that is exactly what day4.probe.log looks like, recorded before v94
    # made the segment cap loud. A verdict that reads only "***" calls that
    # session clean, and it was the one with the bug.
    local -a alarms
    alarms=(${(f)"$(grep -E -e '\*\*\*' -e 'OVERFLOWING' "$log" 2>/dev/null | sed 's/^ *//' | sort -u)"})
    local drops
    drops=$(grep -c "no drawable" "$log" 2>/dev/null)

    print ""
    print "  ── $label ──"
    if (( ${#new} )); then
        print "  *** NEW SITE(S): ${(j:, :)new}"
        print "  *** A latent site went live. Keep $log and the .rec."
    else
        print "  no new uninit sites (only the 3 known)"
    fi
    (( crashes > 0 )) && print "  *** $crashes crash/assert line(s) in the log"
    if (( ${#alarms} )); then
        print "  *** ${#alarms} LOUD ALARM(S) — something was dropped or clamped:"
        print -l -- ${alarms/#/      }
    else
        print "  no loud alarms (no cap overflow, nothing dropped)"
    fi
    (( drops > 0 )) && print "  note: $drops frame(s) dropped for want of a drawable"
    # Render high-water marks, when PC_VTXDIAG was armed. Informational: worth
    # watching climb toward a cap long before it hits one.
    local pv ps
    pv=$(grep -oE 'peak wanted=[0-9]+' "$log" 2>/dev/null | tail -1)
    ps=$(grep -oE 'peak segs wanted=[0-9]+' "$log" 2>/dev/null | tail -1)
    [[ -n "$pv$ps" ]] && print "  render peak: ${pv:-?} ${ps:-}"
    # Heap fullness, loudest first. A heap at 90% is the interesting number —
    # day 4 sat at 89.8% of the old 'teki' budget for weeks and the only signal
    # anyone got was day 5 crashing. Anything at/over 80% gets called out.
    local -a heaps hot
    heaps=(${(f)"$(grep -E '^\[HEAP\]' "$log" 2>/dev/null)"})
    if (( ${#heaps} )); then
        for line in $heaps; do
            # "[HEAP] name  used / size used ( NN.N%) after initStage"
            local hname="${${line#\[HEAP\] }%% *}"
            # sys and ovl read exactly 100% always — measured byte-identical
            # across two different teki sizes (v96), so they are structurally
            # full rather than nearly out. Flagging them every session would
            # train everyone to ignore this line, which is the whole value of it.
            [[ "$hname" == (sys|ovl) ]] && continue
            # FIRST parenthesised figure is the PEAK; a later one on the same
            # line is the instantaneous `used`. Match non-greedily (#, not ##) —
            # greedy grabs `used` and defeats the entire point of the tracker.
            local pct="${${line#*\(}%%\%*}"
            (( ${pct:-0} >= 80 )) && hot+=("${line#\[HEAP\] }")
        done
        if (( ${#hot} )); then
            print "  *** HEAP(S) AT OR ABOVE 80% — headroom is going:"
            print -l -- ${hot/#/      }
        else
            print "  heaps: all below 80% ($(print -l -- $heaps | grep -oE '^\[HEAP\] [a-z]+ ' | wc -l | tr -d ' ') reported)"
        fi
    fi
    grep -E '^\[UNINIT\] (===|[a-z].*hits=)' "$log" 2>/dev/null | sed 's/^/  /'
}

if [[ "${1:-}" == "--summary" ]]; then
    print "Sessions recorded with the probe:"
    setopt null_glob
    logs=("$ROOT"/recordings/*.probe.log)
    unsetopt null_glob
    if (( ! ${#logs} )); then
        print "  (none yet — run tools/probe_session.sh <name>)"
        exit 0
    fi
    for l in $logs; do
        verdict "$l" "${${l:t}%.probe.log}"
    done
    exit 0
fi

name="${1:-}"
build="${2:-build-m1}"
if [[ -z "$name" ]]; then
    print -u2 "usage: tools/probe_session.sh <name> [build-dir]"
    print -u2 "       tools/probe_session.sh --summary"
    exit 2
fi

log="$ROOT/recordings/$name.probe.log"
print "log -> $log"
print ""

# input_record.sh runs ./Pikmin as a child, so an exported PC_UNINIT_PROBE
# reaches it. Cmd-Q is what makes the totals table print (atexit).
#
# PC_VTXDIAG rides along because a play session is the only place the render
# high-water marks are trustworthy — a detached replay draws measurably less
# (v95, and see tools/replay_bounded.sh). Cheap: it prints only when a new
# maximum is set, so it converges to silence within a scene.
#
# PC_HEAPDIAG rides along for the same reason, and it was added the hard way:
# days 5-9 were all played without it, right after day 5 crashed on a full
# enemy heap, so nothing recorded how close any of them came. Recovering it
# meant replaying five recordings. A session that does not measure the thing
# that just broke is the cheapest mistake here to avoid.
PC_UNINIT_PROBE=1 PC_VTXDIAG=1 PC_HEAPDIAG=1 "$ROOT/tools/input_record.sh" "$name" "$build" 2>&1 | tee "$log"

verdict "$log" "$name"
