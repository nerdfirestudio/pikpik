#!/usr/bin/env python3
"""Decode a CMPR (format 14) .bti to PNG and print the Dolphin HD-texture filename.

usage: bti_cmpr.py in.bti out.png
The key is XXH64 (seed 0) of the raw GameCube image bytes, which is what
pc_gx_stub.cpp's pc_hd_lookup matches: tex1_<w>x<h>_<key>_<fmt>.png
"""
import struct, sys, zlib

M = (1 << 64) - 1
P1, P2, P3, P4, P5 = 11400714785074694791, 14029467366897019727, 1609587929392839161, 9650029242287828579, 2870177450012600261
rotl = lambda x, r: ((x << r) | (x >> (64 - r))) & M
def rnd(acc, v): return (rotl((acc + v * P2) & M, 31) * P1) & M
def xxh64(b):
    n, p = len(b), 0
    if n >= 32:
        v = [(P1 + P2) & M, P2, 0, (-P1) & M]
        while p <= n - 32:
            for i in range(4):
                v[i] = rnd(v[i], struct.unpack_from('<Q', b, p)[0]); p += 8
        h = (rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18)) & M
        for x in v: h = (((h ^ rnd(0, x)) * P1) + P4) & M
    else:
        h = P5
    h = (h + n) & M
    while p + 8 <= n:
        h ^= rnd(0, struct.unpack_from('<Q', b, p)[0]); h = (rotl(h, 27) * P1 + P4) & M; p += 8
    if p + 4 <= n:
        h ^= (struct.unpack_from('<I', b, p)[0] * P1) & M; h = (rotl(h, 23) * P2 + P3) & M; p += 4
    while p < n:
        h ^= (b[p] * P5) & M; h = (rotl(h, 11) * P1) & M; p += 1
    h ^= h >> 33; h = (h * P2) & M; h ^= h >> 29; h = (h * P3) & M; h ^= h >> 32
    return h

def c565(c): return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31

def decode(data, w, h):
    out = bytearray(w * h * 4); p = 0
    for ty in range(0, h, 8):
        for tx in range(0, w, 8):
            for sub in range(4):
                bx, by = tx + (sub & 1) * 4, ty + (sub >> 1) * 4
                c0, c1 = struct.unpack_from('>HH', data, p); bits = struct.unpack_from('>I', data, p + 4)[0]; p += 8
                a, b = c565(c0), c565(c1)
                if c0 > c1:
                    pal = [a + (255,), b + (255,), tuple((2*x + y) // 3 for x, y in zip(a, b)) + (255,), tuple((x + 2*y) // 3 for x, y in zip(a, b)) + (255,)]
                else:
                    pal = [a + (255,), b + (255,), tuple((x + y) // 2 for x, y in zip(a, b)) + (255,), (0, 0, 0, 0)]
                for i in range(16):
                    x, y = bx + (i & 3), by + (i >> 2)
                    if x < w and y < h:
                        idx = (bits >> (30 - 2 * i)) & 3
                        out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(pal[idx])
    return bytes(out)

def png(path, w, h, rgba):
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    ch = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw, 9)) + ch(b'IEND', b''))

f = open(sys.argv[1], 'rb').read()
fmt, w, h = f[0], *struct.unpack_from('>HH', f, 2)
off = struct.unpack_from('>I', f, 0x1C)[0]
assert fmt == 14, 'CMPR only'
size = ((w + 7) // 8) * ((h + 7) // 8) * 32
img = f[off:off + size]
png(sys.argv[2], w, h, decode(img, w, h))
print(f'tex1_{w}x{h}_{xxh64(img):016x}_{fmt}.png')
