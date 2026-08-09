#!/usr/bin/env python3
"""Compare our GX texture decode against Dolphin's, per texture format.

Why: a wrong ALPHA channel is invisible on screen until some material happens
to multiply by it. IA8 shipped with intensity/alpha swapped for the whole life
of the port while notes claimed it "verified correct" — because verification
had only ever looked at the colour channels. This diffs both, exhaustively.

Inputs
  ours     build-m1/tex_NNN.png            (PC_DUMP_TEX=1)
  dolphin  Dump/Textures/<GAMEID>/*.png    (Graphics > Advanced > Dump Textures)

Dolphin encodes the GX format in the filename (trailing _N before .png, same
convention as its EFB dumps). Ours are just sequential handles, so pairs are
matched by dimensions + colour-channel fingerprint, then the ALPHA channels are
compared — the channel that hides bugs.

Usage
  python3 tools/tex_audit.py [--ours DIR] [--dolphin DIR] [--verbose]
"""

import argparse
import os
import re
import sys
from collections import defaultdict

try:
    from PIL import Image
except ImportError:
    sys.exit("needs Pillow: python3 -m pip install pillow")

# GX texture format codes as they appear in Dolphin dump filenames.
GX_FORMATS = {
    0: "I4", 1: "I8", 2: "IA4", 3: "IA8", 4: "RGB565", 5: "RGB5A3",
    6: "RGBA8", 8: "C4", 9: "C8", 10: "C14X2", 14: "CMPR",
}


def load(path):
    im = Image.open(path).convert("RGBA")
    return im.size, list(im.getdata())


def fingerprint(px):
    """Cheap colour-only signature: coarse histogram of the RGB channels.
    Deliberately ignores alpha so a texture with a broken alpha channel still
    matches its counterpart (that is the whole point of the audit)."""
    buckets = [0] * 24
    for r, g, b, _ in px:
        buckets[r >> 5] += 1
        buckets[8 + (g >> 5)] += 1
        buckets[16 + (b >> 5)] += 1
    return tuple(buckets)


def channel_stats(px, idx):
    vals = [p[idx] for p in px]
    n = len(vals)
    return {
        "min": min(vals), "max": max(vals),
        "mean": sum(vals) / n,
        "nonzero_pct": 100.0 * sum(1 for v in vals if v) / n,
    }


def compare(a_px, b_px):
    """Per-channel mean absolute difference, 0-255."""
    n = len(a_px)
    out = []
    for idx in range(4):
        diff = sum(abs(a_px[i][idx] - b_px[i][idx]) for i in range(n))
        out.append(diff / n)
    return out


def dolphin_format(name):
    m = re.search(r"_(\d+)\.png$", name)
    return int(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ours", default="build-m1")
    ap.add_argument("--dolphin", default=os.path.expanduser(
        "~/Library/Application Support/Dolphin/Dump/Textures"))
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    ours = sorted(f for f in os.listdir(args.ours)
                  if re.fullmatch(r"tex_\d+\.png", f))
    if not ours:
        sys.exit(f"no tex_NNN.png in {args.ours} — run with PC_DUMP_TEX=1 first")

    dolph = []
    for root, _, files in os.walk(args.dolphin):
        for f in files:
            if f.endswith(".png") and not f.startswith("efb"):
                dolph.append(os.path.join(root, f))
    if not dolph:
        sys.exit(f"no textures under {args.dolphin} — enable Dolphin's "
                 "Graphics > Advanced > Dump Textures and replay the same scenes")

    print(f"ours: {len(ours)} textures   dolphin: {len(dolph)} textures\n")

    # index Dolphin's by (size, colour fingerprint)
    index = {}
    for path in dolph:
        try:
            size, px = load(path)
        except Exception:
            continue
        index.setdefault(size, []).append((path, px))

    per_format = defaultdict(lambda: {"pairs": 0, "rgb": 0.0, "alpha": 0.0,
                                      "worst_alpha": 0.0, "worst_file": ""})
    unmatched = 0
    ambiguous = 0
    unpairable = 0

    for name in ours:
        path = os.path.join(args.ours, name)
        try:
            size, px = load(path)
        except Exception:
            continue
        cands = index.get(size)
        if not cands:
            unmatched += 1
            continue
        # Pick the candidate with the closest COLOUR match, and only accept it
        # as the same texture if the colour channels agree to within rounding.
        # A loose fingerprint match on a small, low-colour texture is otherwise
        # a false pair, and a false pair reports a bogus alpha mismatch.
        # Pairing has to be strict, because the textures this audit cares about
        # most (sheens, decals) are mostly-black in RGB: several DIFFERENT ones
        # then compare equal on colour and get falsely paired, which invents an
        # alpha "mismatch" out of two unrelated images. So demand (a) exact
        # per-pixel colour equality and (b) enough distinct colours that the
        # match is meaningful.
        # Colour-based pairing is only valid if the COLOUR channel actually
        # carries information. Many IA8/I8 sheen textures are uniformly white
        # (or uniformly black) in RGB and differ ONLY in alpha — for those,
        # every same-size candidate scores an identical colour distance and the
        # "winner" is arbitrary. That produced a spurious IA8 failure whose
        # alpha channels correlated at -0.26 (i.e. unrelated images). So: if our
        # RGB is near-uniform, declare the texture unpairable instead.
        vals = [(q[0] + q[1] + q[2]) / 3.0 for q in px]
        mean_v = sum(vals) / len(vals)
        var_v = sum((v - mean_v) ** 2 for v in vals) / len(vals)
        if var_v ** 0.5 < 8.0:
            unpairable += 1
            continue
        # Accept the closest colour match within a few LSBs. Exact equality is
        # too strict: GX bit-expansion (5/4/3-bit channels -> 8) legitimately
        # rounds differently between implementations, so real pairs sit at an
        # RGB delta of ~1-4. Anything past RGB_TOL is a different texture.
        RGB_TOL = 5.0
        scored = []
        for dpath, dpx in cands:
            d = compare(px, dpx)
            scored.append(((d[0] + d[1] + d[2]) / 3.0, d, dpath))
        scored.sort(key=lambda t: t[0])
        if not scored or scored[0][0] > RGB_TOL:
            unmatched += 1
            continue
        if len(scored) > 1 and scored[1][0] <= scored[0][0] * 3.0 + 1.0:
            ambiguous += 1   # runner-up too close to call
            continue
        rgb, d, dpath = scored[0]
        fmt = dolphin_format(os.path.basename(dpath))
        fname = GX_FORMATS.get(fmt, f"fmt{fmt}")
        st = per_format[fname]
        st["pairs"] += 1
        st["rgb"] += rgb
        st["alpha"] += d[3]
        if d[3] > st["worst_alpha"]:
            st["worst_alpha"] = d[3]
            st["worst_file"] = f"{name} vs {os.path.basename(dpath)}"
        if args.verbose:
            print(f"  {name:16s} {size[0]}x{size[1]:<5} {fname:7s} "
                  f"rgb_diff={rgb:6.2f} alpha_diff={d[3]:6.2f}")

    print(f"{'format':10s} {'pairs':>6s} {'rgb diff':>9s} {'alpha diff':>11s}  verdict")
    print("-" * 62)
    bad = []
    for fname, st in sorted(per_format.items()):
        n = st["pairs"]
        rgb, alpha = st["rgb"] / n, st["alpha"] / n
        # A few LSBs of drift is fine (rounding/expansion); structured decode
        # errors land in the tens.
        # Verdict is ALPHA-driven: that is the channel a bug hides in. RGB
        # drift of a few LSBs is expansion rounding, reported but not failed.
        ok = alpha < 2.0
        verdict = "ok" if ok else "MISMATCH"
        if not ok:
            bad.append((fname, st))
        print(f"{fname:10s} {n:6d} {rgb:9.2f} {alpha:11.2f}  {verdict}")

    if unmatched:
        print(f"\n{unmatched} of ours had no exact Dolphin counterpart "
              "(different scenes dumped, or Dolphin skipped a cached texture)")
    if ambiguous:
        print(f"{ambiguous} skipped: runner-up too close to call")
    if unpairable:
        print(f"{unpairable} UNPAIRABLE by this method: their colour channel is "
              "near-uniform\n   (alpha-only sheen textures) — colour cannot "
              "identify which Dolphin texture they are.")
    if bad:
        print("\nMISMATCHES — worst offender per format:")
        for fname, st in bad:
            print(f"  {fname}: alpha diff up to {st['worst_alpha']:.1f}  "
                  f"({st['worst_file']})")
        print("\nA large ALPHA diff with a small RGB diff = channel swap or "
              "wrong bit-expansion in that format's decode\n"
              "(pc_decode_gc_tex in pc/src/pc_gx_stub.cpp).")
        return 1
    print("\nall matched formats decode within rounding tolerance")
    return 0


if __name__ == "__main__":
    sys.exit(main())
