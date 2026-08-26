#!/usr/bin/env python3
"""Pack a compiled sync32 game into a .s32 ROM.
usage: mks32.py game.elf out.s32 --title "Name" --id 8charid [--mode ram|xip] [--video 240|180]
"""
import sys, struct, subprocess, zlib, argparse

def sym_addr(elf, name):
    out = subprocess.check_output(["arm-none-eabi-nm", elf]).decode()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise SystemExit(f"symbol {name} not found")

ap = argparse.ArgumentParser()
ap.add_argument("elf"); ap.add_argument("out")
ap.add_argument("--title", default="untitled")
ap.add_argument("--id", default="00000000")
ap.add_argument("--mode", choices=["ram", "xip"], default="ram")
ap.add_argument("--video", choices=["240", "180"], default="240")
ap.add_argument("--api", type=int, default=1, help="minimum api_version required")
a = ap.parse_args()

img = subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", a.elf, "/dev/stdout"],
                     check=True, capture_output=True).stdout
base = 0x20030000 if a.mode == "ram" else 0x10100000
entry = sym_addr(a.elf, "_start") & ~1   # thumb bit cleared; loader sets it
entry_off = entry - base
assert 0 <= entry_off < len(img), f"entry {entry:#x} outside image"

hdr = struct.pack("<IHHIIIII BBBB 16s 8s 8s".replace(" ", ""),
    0x32335953, 1, a.api,
    64 + len(img), zlib.crc32(img) & 0xffffffff,
    64, len(img), entry_off,
    0 if a.mode == "ram" else 1,
    0 if a.video == "240" else 1,
    0, 0,
    a.title.encode()[:16].ljust(16, b"\0"),
    a.id.encode()[:8].ljust(8, b"\0"),
    b"\0" * 8)
assert len(hdr) == 64, len(hdr)
open(a.out, "wb").write(hdr + img)
print(f"{a.out}: {len(img)} byte image, mode={a.mode}, entry_off={entry_off:#x}, title={a.title!r}")
