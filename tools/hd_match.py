#!/usr/bin/env python3
"""Match a Dolphin HD texture pack against textures dumped from this port.

    PC_DUMP_TEXHASH=/tmp/texcorpus ./Pikmin          # collect
    tools/hd_match.py /tmp/texcorpus references/PikminHD_v0.1b  > hd_manifest.txt

WHY THIS EXISTS INSTEAD OF JUST HASHING LIKE DOLPHIN DOES
---------------------------------------------------------
Dolphin names pack files tex1_<w>x<h>_<hash>_<fmt>.png, so the obvious
integration is to compute that hash and look the filename up. That does not
work: Dolphin's texture hash is a CRC32-based routine seeded with the length,
with CPU-dispatched implementations, and its sampling behaviour depends on the
user's "safe texture cache" setting. Reproducing it means reproducing one
specific build's behaviour, and a mismatch is SILENT — no replacement, no
explanation. Measured: none of full XXH64, XXH64 seeded with length, or sampled
variants at 128/512 reproduced any pack hash. See docs/ARCHITECTURE.md.

So the matching happens ONCE, here, by content:

  1. group both sides by (width, height, format) — formats use the same
     numbering as pc_decode_gc_tex, which is also what Dolphin puts in the name
  2. decode our tiled GC texture to luma, downscale the candidate PNG to the
     same size, and score by Pearson correlation
  3. emit a manifest keyed by OUR OWN hash of the raw bytes (FNV-1a 64 — a few
     lines in both Python and C++, so the renderer needs no crypto code)

Both sides of that key are then ours, and Dolphin never enters the picture.
A true match scores far above the noise: the validating case (504x1008 IA4,
the font atlas, one candidate) scores r = 0.82 while unrelated pairs of the
same size sit near zero.
"""

import os
import re
import sys
from collections import defaultdict

try:
    from PIL import Image
except ImportError:
    sys.exit("needs Pillow:  pip install pillow")

# GC tile geometry: (tile_w, tile_h, bytes_per_tile). Same table as
# pc_decode_gc_tex in pc/src/pc_gx_stub.cpp.
TILES = {
    0x0: (8, 8, 32),   # I4
    0x1: (8, 4, 32),   # I8
    0x2: (8, 4, 32),   # IA4
    0x3: (4, 4, 32),   # IA8
    0x4: (4, 4, 32),   # RGB565
    0x5: (4, 4, 32),   # RGB5A3
    0x6: (4, 4, 64),   # RGBA8
    0xE: (8, 8, 32),   # CMPR
}

FMT_NAMES = {0x0: "I4", 0x1: "I8", 0x2: "IA4", 0x3: "IA8", 0x4: "RGB565",
             0x5: "RGB5A3", 0x6: "RGBA8", 0xE: "CMPR"}


def _luma565(p):
    r = ((p >> 11) & 0x1F) * 255 // 31
    g = ((p >> 5) & 0x3F) * 255 // 63
    b = (p & 0x1F) * 255 // 31
    return (r * 299 + g * 587 + b * 114) // 1000


def decode_luma(data, w, h, fmt):
    """Decode a tiled GC texture to a flat list of luma bytes, row-major w*h.

    Luma only — this is for correlation, not display, and it keeps the CMPR
    path (the pack's most common format by far) tractable.
    """
    if fmt not in TILES:
        return None
    tw, th, _ = TILES[fmt]
    out = bytearray(w * h)
    p = 0

    for ty in range(0, h, th):
        for tx in range(0, w, tw):
            if fmt == 0xE:
                # CMPR tile = four DXT1 sub-blocks, in the order
                # (0,0) (4,0) (0,4) (4,4). Endpoints are BIG-ENDIAN RGB565.
                for sy, sx in ((0, 0), (0, 4), (4, 0), (4, 4)):
                    c0 = (data[p] << 8) | data[p + 1]
                    c1 = (data[p + 2] << 8) | data[p + 3]
                    bits = data[p + 4:p + 8]
                    p += 8
                    l0, l1 = _luma565(c0), _luma565(c1)
                    if c0 > c1:
                        lut = (l0, l1, (2 * l0 + l1) // 3, (l0 + 2 * l1) // 3)
                    else:
                        lut = (l0, l1, (l0 + l1) // 2, 0)
                    for row in range(4):
                        byte = bits[row]
                        for col in range(4):
                            idx = (byte >> (6 - col * 2)) & 3
                            x, y = tx + sx + col, ty + sy + row
                            if x < w and y < h:
                                out[y * w + x] = lut[idx]
            else:
                for y in range(th):
                    for x in range(tw):
                        if fmt == 0x0:      # I4 — two texels per byte
                            b = data[p + (y * tw + x) // 2]
                            v = ((b >> 4) if (x & 1) == 0 else (b & 0xF)) * 17
                        elif fmt == 0x1:    # I8
                            v = data[p + y * tw + x]
                        elif fmt == 0x2:    # IA4 — low nibble intensity
                            v = (data[p + y * tw + x] & 0xF) * 17
                        elif fmt == 0x3:    # IA8 — byte0 alpha, byte1 intensity
                            v = data[p + (y * tw + x) * 2 + 1]
                        elif fmt == 0x4:    # RGB565
                            o = p + (y * tw + x) * 2
                            v = _luma565((data[o] << 8) | data[o + 1])
                        elif fmt == 0x5:    # RGB5A3
                            o = p + (y * tw + x) * 2
                            px = (data[o] << 8) | data[o + 1]
                            if px & 0x8000:
                                r = ((px >> 10) & 0x1F) * 255 // 31
                                g = ((px >> 5) & 0x1F) * 255 // 31
                                b = (px & 0x1F) * 255 // 31
                            else:
                                r = ((px >> 8) & 0xF) * 17
                                g = ((px >> 4) & 0xF) * 17
                                b = (px & 0xF) * 17
                            v = (r * 299 + g * 587 + b * 114) // 1000
                        elif fmt == 0x6:    # RGBA8 — AR block then GB block
                            o = p + (y * tw + x) * 2
                            r = data[o + 1]
                            g = data[p + 32 + (y * tw + x) * 2]
                            b = data[p + 32 + (y * tw + x) * 2 + 1]
                            v = (r * 299 + g * 587 + b * 114) // 1000
                        else:
                            v = 0
                        xx, yy = tx + x, ty + y
                        if xx < w and yy < h:
                            out[yy * w + xx] = v
                p += TILES[fmt][2]
    return out


def fnv1a64(data):
    """FNV-1a 64. Must stay identical to pc_hd_hash() in pc/src/pc_gx_stub.cpp."""
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h


def pearson(a, b):
    n = len(a)
    if n == 0:
        return 0.0
    ma, mb = sum(a) / n, sum(b) / n
    num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    da = sum((x - ma) ** 2 for x in a)
    db = sum((y - mb) ** 2 for y in b)
    return num / ((da * db) ** 0.5) if da and db else 0.0


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    dump_dir, pack_dir = sys.argv[1], sys.argv[2]
    # 0.70 default. The validating case (the font atlas, one candidate) scores
    # 0.825 and genuine matches cluster at 0.73+, while same-size unrelated
    # pairs sit near zero and the marginal band around 0.50-0.62 turned out to
    # be dominated by near-identical animation frames matching each other's
    # neighbour. Luma-only correlation cannot separate those, so exclude them
    # rather than ship a wrong replacement.
    threshold = float(sys.argv[3]) if len(sys.argv) > 3 else 0.70

    pack = defaultdict(list)
    for f in os.listdir(pack_dir):
        m = re.match(r"tex1_(\d+)x(\d+)_([0-9a-f]+)_(\d+)\.png", f)
        if m:
            pack[(int(m[1]), int(m[2]), int(m[4]))].append(f)

    ours = []
    for f in sorted(os.listdir(dump_dir)):
        m = re.match(r"(\d+)x(\d+)_(\d+)_(\d+)\.bin", f)
        if m:
            ours.append((int(m[1]), int(m[2]), int(m[3]), f))

    print("# HD texture manifest — generated by tools/hd_match.py")
    print("# <FNV-1a 64 of raw tiled bytes>  <pack png>  # WxH fmt r=score")
    matched = unmatched = 0
    log = []
    claimed = {}

    for w, h, fmt, fn in ours:
        cands = pack.get((w, h, fmt))
        if not cands:
            unmatched += 1
            continue
        data = open(os.path.join(dump_dir, fn), "rb").read()
        mine = decode_luma(data, w, h, fmt)
        if mine is None:
            continue
        best, best_r = None, -2.0
        for c in cands:
            img = Image.open(os.path.join(pack_dir, c)).convert("L").resize((w, h), Image.LANCZOS)
            r = pearson(mine, list(img.getdata()))
            if r > best_r:
                best, best_r = c, r
        if best_r >= threshold:
            digest = fnv1a64(data)
            claimed.setdefault(best, []).append((digest, w, h, fmt, best_r, fn))
            matched += 1
        else:
            log.append(f"#   NO MATCH {fn} ({w}x{h} {FMT_NAMES.get(fmt,fmt)}) "
                       f"best={best} r={best_r:.3f} of {len(cands)} candidates")
            unmatched += 1

    # One HD texture can only replace ONE original. If several of ours claim the
    # same pack file they are near-identical (animation frames, palette
    # variants) and luma correlation cannot tell them apart — emitting any of
    # them would swap the wrong texture, so drop the lot and say so.
    emitted = 0
    for pngname, claims in sorted(claimed.items()):
        if len(claims) > 1:
            best = max(claims, key=lambda c: c[4])
            log.append(f"#   AMBIGUOUS {pngname} claimed by {len(claims)} textures "
                       f"(r={', '.join(f'{c[4]:.3f}' for c in sorted(claims, key=lambda c: -c[4]))}) — dropped")
            continue
        d, w, h, fmt, r, fn = claims[0]
        print(f"{d}  {pngname}  # {w}x{h} {FMT_NAMES.get(fmt, fmt)} r={r:.3f}")
        emitted += 1

    print(f"# emitted {emitted} of {matched} above threshold; "
          f"{unmatched} unmatched; pack has {sum(len(v) for v in pack.values())} entries")
    for line in log:
        print(line)


if __name__ == "__main__":
    main()
