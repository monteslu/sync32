#!/usr/bin/env python3
"""Drop files onto the sync32's SD card over SWD (openocd + debugprobe).
usage: tools_drop.py <elf-for-symbols> <local-file> <dest-name> [more pairs...]"""
import subprocess, sys, zlib, struct, tempfile, os

OCD = os.path.expanduser("~/code/pico/openocd-install/bin/openocd")
CFG = ["-f", "interface/cmsis-dap.cfg", "-f", "target/rp2350.cfg", "-c", "adapter speed 4000"]
MB_OPEN, MB_DATA, MB_CLOSE = 1, 2, 3
BUF = 0x20068000
CHUNK = 16384

def ocd(cmds):
    r = subprocess.run([OCD] + CFG + ["-c", "init"] + sum([["-c", c] for c in cmds], []) + ["-c", "shutdown"],
                       capture_output=True, text=True, timeout=180)
    return r.stdout + r.stderr

def sym(elf, name):
    out = subprocess.check_output(["arm-none-eabi-nm", elf]).decode()
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    raise SystemExit(f"symbol {name} not found in {elf}")

def wait_idle(mbox, tries=60):
    for _ in range(tries):
        out = ocd([f"mdw {mbox+4:#x}"])
        for line in out.splitlines():
            if line.startswith(f"{mbox+4:#010x}".replace("0X","0x")) or f"{mbox+4:08x}" in line:
                v = int(line.split()[-1], 16)
                if v == 0:
                    return True
    return False

def status(mbox):
    out = ocd([f"mdw {mbox+8:#x}"])
    for line in out.splitlines():
        if f"{mbox+8:08x}" in line:
            v = int(line.split()[-1], 16)
            return v - (1 << 32) if v >= (1 << 31) else v
    return None

def send(elf, path, dest):
    mbox = sym(elf, "s32_xfer_mbox")
    data = open(path, "rb").read()
    print(f"  mailbox @ {mbox:#x}, {len(data)} bytes -> {dest}")
    # MB_OPEN with name
    name = dest.encode() + b"\0"
    name += b"\0" * ((4 - len(name) % 4) % 4)
    cmds = []
    for i in range(0, len(name), 4):
        w = struct.unpack("<I", name[i:i+4])[0]
        cmds.append(f"mww {mbox+20+i:#x} {w:#x}")
    cmds.append(f"mww {mbox+4:#x} {MB_OPEN}")
    ocd(cmds)
    if not wait_idle(mbox): raise SystemExit("open: no response")
    st = status(mbox)
    if st != 0: raise SystemExit(f"open failed status={st}")
    # data chunks
    sent = 0
    with tempfile.TemporaryDirectory() as td:
        for off in range(0, len(data), CHUNK):
            blk = data[off:off+CHUNK]
            f = os.path.join(td, "blk.bin")
            open(f, "wb").write(blk)
            crc = zlib.crc32(blk) & 0xffffffff
            ocd([f"load_image {f} {BUF:#x} bin",
                 f"mww {mbox+12:#x} {len(blk)}",
                 f"mww {mbox+16:#x} {crc:#x}",
                 f"mww {mbox+4:#x} {MB_DATA}"])
            if not wait_idle(mbox): raise SystemExit("data: no response")
            st = status(mbox)
            if st != 0: raise SystemExit(f"data failed status={st} at {off}")
            sent += len(blk)
            print(f"    {sent}/{len(data)}", end="\r", flush=True)
    ocd([f"mww {mbox+4:#x} {MB_CLOSE}"])
    if not wait_idle(mbox): raise SystemExit("close: no response")
    st = status(mbox)
    if st != 0: raise SystemExit(f"close failed status={st}")
    print(f"    {sent}/{len(data)} OK          ")

if __name__ == "__main__":
    elf = sys.argv[1]
    args = sys.argv[2:]
    for i in range(0, len(args), 2):
        print(f"sending {args[i]}")
        send(elf, args[i], args[i+1])
    print("done")
