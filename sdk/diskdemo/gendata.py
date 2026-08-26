#!/usr/bin/env python3
# Deterministic test data: PCG32 keyed per 4KB block (must match main.c).
import sys, struct

def pcg_out(s):
    x = (((s >> 18) ^ s) >> 27) & 0xffffffff
    r = s >> 59
    return ((x >> r) | (x << ((32 - r) & 31))) & 0xffffffff

M, I = 6364136223846793005, 1442695040888963407
out, total = sys.argv[1], int(sys.argv[2])
BLOCK = 4096
with open(out, "wb") as f:
    for blk in range(total // BLOCK):
        s = (0 * M + I) & (2**64 - 1)
        s = (s + 0xD15CDA7A00 + blk) & (2**64 - 1)
        s = (s * M + I) & (2**64 - 1)
        words = []
        for _ in range(BLOCK // 4):
            old = s
            s = (s * M + I) & (2**64 - 1)
            words.append(pcg_out(old))
        f.write(struct.pack("<%dI" % len(words), *words))
print(f"{out}: {total} bytes")
