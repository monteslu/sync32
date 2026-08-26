#!/usr/bin/env python3
"""Dump the console's persistent event log over SWD (survives warm reboots).
usage: s32log.py [--elf firmware/build-mac/sync32.elf]
"""
import argparse, pathlib, re, subprocess, sys

LOG_SIZE = 2048  # must match log.c


def sym_addr(elf, name):
    out = subprocess.check_output(["arm-none-eabi-nm", elf]).decode()
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    sys.exit(f"symbol {name} not found")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=str(pathlib.Path(__file__).resolve()
                    .parents[2] / "firmware/build-mac/sync32.elf"))
    a = ap.parse_args()
    ring = sym_addr(a.elf, "ring")
    total = 16 + LOG_SIZE
    out = subprocess.run(
        ["openocd", "-f", "interface/cmsis-dap.cfg", "-f", "target/rp2350.cfg",
         "-c", "adapter speed 5000", "-c", "init",
         "-c", f'echo "RING [read_memory {ring:#x} 8 {total}]"', "-c", "shutdown"],
        capture_output=True, text=True)
    m = re.search(r"RING ((?:0x[0-9a-f]+ ?)+)", out.stdout + out.stderr)
    if not m:
        sys.exit("could not read ring (console rebooting? probe busy?)")
    data = bytes(int(x, 16) for x in m.group(1).split())
    magic, seq, head, boot = (int.from_bytes(data[i:i+4], "little")
                              for i in range(0, 16, 4))
    if magic != 0x53324C47:
        sys.exit(f"log magic invalid ({magic:#x}): cold RAM or old firmware")
    buf = data[16:16 + LOG_SIZE]
    print(f"# boot={boot} seq={seq}")
    i = (head + 1) % LOG_SIZE
    text = []
    while i != head:
        if buf[i]:
            text.append(chr(buf[i]))
        i = (i + 1) % LOG_SIZE
    sys.stdout.write("".join(text))


if __name__ == "__main__":
    main()
