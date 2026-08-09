#!/usr/bin/env python3
"""Decide the AFC decode variant offline, against real .aw wave data.

The DSP ucode's AFC decoder is not in main.dol, so it cannot be read from the
oracle the way CPU-side code can. This picks the variant empirically instead:
a correct ADPCM decode of real audio is smooth and never clamps; a wrong one
is noisy, or saturates on a large fraction of samples.

IMPORTANT: run this on a REAL AFC source, not on a .aw file. Pikmin's .aw
banks are largely PCM8 (samplesSourceType 8); decoding those as AFC produces
noise and makes the scoring meaningless. Dump a genuine AFC voice from ARAM
with PC_DUMP_AFC=<file> and probe that.

Equally important: SMOOTHNESS ALONE DOES NOT DISCRIMINATE. A wrong predictor
integrates, giving output that is very smooth but is mostly DC wander. Score
on drift relative to RMS.

Result for Pikmin (GPIE01_01), boot voice dumped from ARAM:

    variant             mean   maxDCdrift      rms
    scale=hi shifted   -16.2        142.5   9570.3   <- correct
    scale=hi delta*n  -240.5       1154.3   1146.3   drift ~= signal
    scale=lo shifted   1662.9      8731.7  18422.0   clamps 10%
    scale=lo delta*n  -4252.1     23191.2  19095.4   clamps 15%

i.e. the header's HIGH nibble is the scale and the LOW nibble is the
coefficient index, and the sample term is (nibble << scale) << 11.

Usage: afc_probe.py <path-to-raw-afc>
"""
import math
import sys

# DSPADPCM_FILTER from src/jaudio/dspinterface.c — u32 entries packing two
# 1.11 coefficients each, c1 in the high half.
COEF_U32 = [0x00000000, 0x08000000, 0x00000800, 0x04000400, 0x1000f800, 0x0e00fa00, 0x0c00fc00, 0x1200f600,
            0x1068f738, 0x12c0f704, 0x1400f400, 0x0800f800, 0x0400fc00, 0xfc000400, 0xfc000000, 0xf8000000]


def s16(x):
    x &= 0xFFFF
    return x - 0x10000 if x >= 0x8000 else x


COEF = [(s16(v >> 16), s16(v & 0xFFFF)) for v in COEF_U32]


def decode(buf, start, nblocks, hi_is_scale, shift_form):
    yn1 = yn2 = 0
    out = []
    clamp = 0
    off = start
    for _ in range(nblocks):
        if off + 9 > len(buf):
            break
        h = buf[off]
        if hi_is_scale:
            scale, idx = (h >> 4) & 0xF, h & 0xF
        else:
            idx, scale = (h >> 4) & 0xF, h & 0xF
        c1, c2 = COEF[idx]
        nib = []
        for i in range(8):
            by = buf[off + 1 + i]
            for n in ((by >> 4) & 0xF, by & 0xF):
                nib.append(n - 16 if n >= 8 else n)
        for n in nib:
            samp = ((n << scale) << 11) if shift_form else ((1 << scale) * n)
            samp += yn1 * c1 + yn2 * c2
            samp >>= 11
            if samp > 32767:
                samp, clamp = 32767, clamp + 1
            elif samp < -32768:
                samp, clamp = -32768, clamp + 1
            out.append(samp)
            yn2, yn1 = yn1, samp
        off += 9
    return out, clamp


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'build-m1/gamedata/dataDir/SndData/Banks/pikise_0.aw'
    data = open(path, 'rb').read()
    print('%-26s %7s %8s %8s' % ('variant', 'clamp%', 'HF/RMS', 'peak'))
    for hi_is_scale in (True, False):
        for shift_form in (True, False):
            out, cl = decode(data, 0, 1200, hi_is_scale, shift_form)
            rms = math.sqrt(sum(v * v for v in out) / len(out)) or 1
            hf = sum(abs(out[i] - out[i - 1]) for i in range(1, len(out))) / (len(out) - 1)
            name = 'scale=%s %s' % ('hi' if hi_is_scale else 'lo', 'shifted' if shift_form else 'delta*n')
            print('%-26s %6.2f%% %8.4f %8d' % (name, 100 * cl / len(out), hf / rms, max(abs(v) for v in out)))


if __name__ == '__main__':
    main()
