#!/usr/bin/env python3
"""Import a Dolphin/GameCube .gci memory-card file into this port's card dir.

    tools/gci_import.py <file.gci> [card-dir]        # default build-m1/save/card_a
    tools/gci_import.py <file.gci> --dry-run

Why this exists: reaching the final boss to test late-game AI is many hours of
play, but a .gci save gets there instantly. The formats are close enough that
this is a header parse, not a conversion — a .gci is a 64-byte CARDDir followed
by the raw file payload, and that payload IS byte-for-byte what we store as
<name>.bin. Only the metadata sidecar has to be rebuilt.

Layouts, both verified against real files rather than assumed:

  .gci header = CARDDir (include/Dolphin/card.h:76), BIG-endian, 0x40 bytes
    0x00 gameName[4]   0x04 company[2]   0x07 bannerFormat   0x08 fileName[32]
    0x28 time u32      0x2C iconAddr u32 0x30 iconFormat u16 0x32 iconSpeed u16
    0x34 permission    0x38 length u16 (in BLOCKS)           0x3C commentAddr u32

  <name>.stat = PcCardStatFile (pc/src/pc_card.cpp:204), HOST byte order, 36 bytes
    magic 'PCM1' | fileNo | gameName[4] | company[2] | bannerFormat | permission
    | time | iconAddr | iconFormat | iconSpeed | commentAddr | length IN BYTES

The two length fields differ in unit — blocks on the card, bytes in our sidecar
— which is the one thing here that will silently produce a wrong save if missed.
"""
import os
import struct
import sys

CARD_BLOCK = 8192
STAT_MAGIC = 0x50434D31  # 'PCM1'
# PcCardStatFile, host byte order. Explicit sizes rather than native alignment so
# this cannot drift with struct padding.
STAT_FMT = "<Ii4s2sBBIIHHII"
HDR = 0x40


def parse_gci(raw, path):
    if len(raw) < HDR:
        sys.exit("%s: too small to be a .gci (%d bytes)" % (path, len(raw)))

    game = raw[0x00:0x04]
    company = raw[0x04:0x06]
    banner = raw[0x07]
    name = raw[0x08:0x28].split(b"\0", 1)[0].decode("ascii", "replace")
    (time,) = struct.unpack_from(">I", raw, 0x28)
    (icon_addr,) = struct.unpack_from(">I", raw, 0x2C)
    (icon_fmt,) = struct.unpack_from(">H", raw, 0x30)
    (icon_speed,) = struct.unpack_from(">H", raw, 0x32)
    perm = raw[0x34]
    (blocks,) = struct.unpack_from(">H", raw, 0x38)
    (comment,) = struct.unpack_from(">I", raw, 0x3C)

    payload = raw[HDR:]
    expect = blocks * CARD_BLOCK
    if len(payload) != expect:
        # Refuse rather than write a truncated save: a short payload loads as a
        # corrupt file and the failure surfaces much later, in-game.
        sys.exit("%s: payload is %d bytes but the header claims %d blocks (%d bytes)"
                 % (path, len(payload), blocks, expect))

    return dict(game=game, company=company, banner=banner, name=name, time=time,
                icon_addr=icon_addr, icon_fmt=icon_fmt, icon_speed=icon_speed,
                perm=perm, comment=comment, payload=payload, nbytes=expect)


def build_stat(d, file_no=0):
    # Host byte order and exact field order of PcCardStatFile. '<' plus explicit
    # sizes rather than native alignment, so this cannot drift with padding.
    return struct.pack(STAT_FMT,
                       STAT_MAGIC, file_no, d["game"], d["company"], d["banner"],
                       d["perm"], d["time"], d["icon_addr"], d["icon_fmt"],
                       d["icon_speed"], d["comment"], d["nbytes"])


def main():
    args = [a for a in sys.argv[1:] if a != "--dry-run"]
    dry = "--dry-run" in sys.argv[1:]
    if not args:
        sys.exit(__doc__)

    gci = args[0]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    dest = args[1] if len(args) > 1 else os.path.join(root, "build-m1", "save", "card_a")

    d = parse_gci(open(gci, "rb").read(), gci)
    stat = build_stat(d)
    if len(stat) != 36:
        sys.exit("internal: stat sidecar is %d bytes, expected 36" % len(stat))

    print("%s" % gci)
    print("  file      : %r" % d["name"])
    print("  game/co   : %s / %s" % (d["game"].decode(), d["company"].decode()))
    print("  payload   : %d bytes (%d blocks)" % (d["nbytes"], d["nbytes"] // CARD_BLOCK))
    print("  -> %s/%s.bin  + .stat" % (dest, d["name"]))

    if dry:
        print("  (dry run, nothing written)")
        return

    os.makedirs(dest, exist_ok=True)
    binp = os.path.join(dest, d["name"] + ".bin")
    stp = os.path.join(dest, d["name"] + ".stat")
    # Never clobber silently — a card dir is play state, and overwriting the
    # wrong one costs a session.
    for p in (binp, stp):
        if os.path.exists(p):
            bak = p + ".preimport"
            os.replace(p, bak)
            print("  existing %s -> %s" % (os.path.basename(p), os.path.basename(bak)))
    open(binp, "wb").write(d["payload"])
    open(stp, "wb").write(stat)
    print("  written.")


if __name__ == "__main__":
    main()
