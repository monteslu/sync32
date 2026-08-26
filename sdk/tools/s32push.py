#!/usr/bin/env python3
"""Push files onto the sync32 SD card over SWD (debugprobe + openocd).
The console must be showing the launcher menu (it polls the mailbox there).

usage: s32push.py [--elf firmware/build-mac/sync32.elf] file [file...]
       s32push.py --dest nesf/ roms/*.nes      # into a subdir on the card
Each file lands as its basename (prefixed by --dest), written atomically.
"""
import argparse, socket, subprocess, sys, time, zlib, pathlib, tempfile, os

CHUNK = 16384            # must match MBOX_BUF_MAX in xfer.c
BUF_ADDR = 0x20068000    # must match MBOX_BUF in xfer.c
MB_OPEN, MB_DATA, MB_CLOSE, MB_ABORT = 1, 2, 3, 4
# struct s32_xfer_mbox_t field offsets
O_MAGIC, O_CMD, O_STATUS, O_LEN, O_CRC, O_NAME = 0, 4, 8, 12, 16, 20
MAGIC = 0x53325846


def sym_addr(elf, name):
    out = subprocess.check_output(["arm-none-eabi-nm", elf]).decode()
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    sys.exit(f"symbol {name} not found in {elf}")


class OpenOCD:
    def __init__(self):
        self.proc = subprocess.Popen(
            ["openocd", "-f", "interface/cmsis-dap.cfg", "-f", "target/rp2350.cfg",
             "-c", "adapter speed 5000", "-c", "tcl_port 6666", "-c", "gdb_port disabled",
             "-c", "telnet_port disabled", "-c", "init"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(50):
            try:
                self.s = socket.create_connection(("127.0.0.1", 6666), timeout=1)
                break
            except OSError:
                time.sleep(0.2)
        else:
            sys.exit("openocd tcl port never came up")

    def cmd(self, c):
        self.s.sendall(c.encode() + b"\x1a")
        buf = b""
        while not buf.endswith(b"\x1a"):
            buf += self.s.recv(4096)
        return buf[:-1].decode()

    def mww(self, addr, val):
        self.cmd(f"mww {addr:#x} {val:#x}")

    def mdw(self, addr):
        r = self.cmd(f"read_memory {addr:#x} 32 1").strip()
        return int(r.split()[0], 16)

    def write_block(self, addr, data):
        with tempfile.NamedTemporaryFile(delete=False, suffix=".bin") as f:
            f.write(data)
            tmp = f.name
        self.cmd(f"load_image {tmp} {addr:#x} bin")
        os.unlink(tmp)

    def close(self):
        try:
            self.cmd("shutdown")
        except Exception:
            pass
        self.proc.wait(timeout=5)


def wait_idle(ocd, mbox, timeout=8.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if ocd.mdw(mbox + O_CMD) == 0:
            st = ocd.mdw(mbox + O_STATUS)
            return st - (1 << 32) if st >= 1 << 31 else st
        time.sleep(0.01)
    sys.exit("mailbox timeout: is the console on the launcher menu?")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=str(pathlib.Path(__file__).resolve()
                    .parents[2] / "firmware/build-mac/sync32.elf"))
    ap.add_argument("--dest", default="", help="subdir on the card, e.g. nesf/")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()

    mbox = sym_addr(a.elf, "s32_xfer_mbox")
    ocd = OpenOCD()
    try:
        if ocd.mdw(mbox + O_MAGIC) != MAGIC:
            sys.exit("mailbox magic missing: firmware without xfer support?")
        for path in a.files:
            data = open(path, "rb").read()
            name = (a.dest.rstrip("/") + "/" if a.dest else "") + pathlib.Path(path).name
            nb = name.encode()[:63] + b"\0"
            ocd.write_block(mbox + O_NAME, nb)
            ocd.mww(mbox + O_CMD, MB_OPEN)
            st = wait_idle(ocd, mbox)
            if st != 0:
                sys.exit(f"{name}: open failed {st}")
            t0 = time.time()
            for off in range(0, len(data), CHUNK):
                chunk = data[off:off + CHUNK]
                ocd.write_block(BUF_ADDR, chunk)
                ocd.mww(mbox + O_LEN, len(chunk))
                ocd.mww(mbox + O_CRC, zlib.crc32(chunk) & 0xFFFFFFFF)
                ocd.mww(mbox + O_CMD, MB_DATA)
                st = wait_idle(ocd, mbox)
                if st != 0:
                    ocd.mww(mbox + O_CMD, MB_ABORT)
                    wait_idle(ocd, mbox)
                    sys.exit(f"{name}: chunk @{off} failed {st}")
            ocd.mww(mbox + O_CMD, MB_CLOSE)
            st = wait_idle(ocd, mbox)
            if st != 0:
                sys.exit(f"{name}: close failed {st}")
            dt = time.time() - t0
            kbs = len(data) / 1024 / dt if dt else 0
            print(f"{name}: {len(data)} bytes in {dt:.1f}s ({kbs:.0f} KB/s)")
    finally:
        ocd.close()


if __name__ == "__main__":
    main()
