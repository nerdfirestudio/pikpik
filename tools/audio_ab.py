#!/usr/bin/env python3
"""A/B our audio output against a Dolphin reference dump.

The M5 plan specifies this as the acceptance instrument and it is the only way
to tell "this sound is wrong" from "this sound is legitimately harsh" — an ear
cannot, and neither can a smoothness metric (see tools/afc_probe.py for how a
plausible-looking metric picked a broken decoder).

Capturing the Dolphin side:

    # back up first — this edits the user's Dolphin config
    cp ~/Library/Application\\ Support/Dolphin/Config/Dolphin.ini /tmp/
    # add under [DSP]:  DumpAudio = True
    /Applications/Dolphin.app/Contents/MacOS/Dolphin -b -e "<path to iso>"
    # dumps land in ~/Library/Application Support/Dolphin/Dump/Audio/
    #   *_dspdump.wav  = the DSP mix (what our renderer produces)
    #   *_dtkdump.wav  = DTK streaming audio (separate path)
    # then RESTORE the config

Capturing ours:

    cd build-m1 && PC_AUDIO=1 PC_DUMP_WAV=out.wav ./Pikmin

Note the two will not be sample-aligned — boot timing differs by around a
second — so compare the per-second RMS/ZCR PROFILE and the overall peak, not
sample-by-sample. Boot->title needs no input on either side, which is what
makes it automatable; menus need controller input in Dolphin.

Result at the time of writing (boot -> title): peak 9432 vs 9422, and the
boot ident matched at rms 2636/2631, zcr 0.095/0.089.

Usage: audio_ab.py <dolphin_dspdump.wav> <ours.wav>
"""
import array
import math
import sys
import wave


def profile(path, seconds=20):
    w = wave.open(path, 'rb')
    ch = w.getnchannels()
    r = w.getframerate()
    n = w.getnframes()
    d = array.array('h')
    d.frombytes(w.readframes(n))
    left = d[0::ch]
    rows = []
    for t in range(min(seconds, len(left) // r)):
        seg = left[t * r:(t + 1) * r]
        if not seg:
            break
        rms = math.sqrt(sum(v * v for v in seg) / len(seg))
        zc = sum(1 for i in range(1, len(seg)) if (seg[i - 1] < 0) != (seg[i] < 0)) / len(seg)
        rows.append((t, rms, zc))
    return rows, r, (max(abs(x) for x in left) if left else 0)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    a, ra, pa = profile(sys.argv[1])
    b, rb, pb = profile(sys.argv[2])
    print('dolphin: %d Hz peak %d     ours: %d Hz peak %d' % (ra, pa, rb, pb))
    print(' t   dolphin rms/zcr      ours rms/zcr')
    for i in range(min(len(a), len(b))):
        print('%2d   %7.0f %.3f      %7.0f %.3f' % (a[i][0], a[i][1], a[i][2], b[i][1], b[i][2]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
