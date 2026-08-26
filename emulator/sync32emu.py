#!/usr/bin/env python3
"""sync32 emulator v0.1: HLE. Emulates one Cortex-M33 (Thumb) via Unicorn,
implements the sync32 ABI natively. Same .s32 file as real hardware.

usage: sync32emu.py game.s32 [--scale N] [--frames N] [--screenshot out.png]
                             [--headless] [--turbo] [--seed N]
keys: arrows=dpad  Z=A X=B A=X S=Y  Q=L W=R  Enter=Start RShift=Select  Esc=quit
"""
import sys, struct, time, zlib, argparse, pathlib
import numpy as np
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
    UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC)

SRAM_BASE, SRAM_SIZE = 0x20000000, 0x82000
FLASH_BASE, FLASH_SIZE = 0x10000000, 16 * 1024 * 1024
GAME_RAM, GAME_STACK_TOP = 0x20030000, 0x20080000
XIP_SLOT = 0x10100000
FB_ADDR = 0x20008000                       # canvas/framebuffer in emulated RAM
API_PAGE, API_TABLE, API_FUNCS = 0x04000000, 0x04000100, 0x04000800

class Sync32Emu:
    def __init__(self, rom_path, args):
        self.args = args
        rom = open(rom_path, "rb").read()
        (magic, hver, aver, size, crc, coff, csize, eoff, lmode, vmode,
         flags, _r) = struct.unpack_from("<IHHIIIIIBBBB", rom, 0)
        assert magic == 0x32335953, "bad magic"
        assert size == len(rom) and zlib.crc32(rom[64:]) & 0xffffffff == crc, "bad crc"
        self.title = rom[0x20:0x30].rstrip(b"\0").decode()
        self.game_id = rom[0x30:0x38].hex()
        self.video_mode = vmode
        img = rom[coff:coff + csize]

        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.mem_map(SRAM_BASE, SRAM_SIZE)
        uc.mem_map(FLASH_BASE, FLASH_SIZE)
        uc.mem_map(API_PAGE, 0x1000)
        if lmode == 0:
            base = GAME_RAM
            uc.mem_write(base, img)
        else:
            base = XIP_SLOT
            uc.mem_write(base, img)
        self.entry = base + eoff

        # api table: u16 version, u16 pad, 18 function pointers
        n_funcs = 18
        tbl = struct.pack("<HH", 1, 0) + b"".join(
            struct.pack("<I", (API_FUNCS + i * 4) | 1) for i in range(n_funcs))
        uc.mem_write(API_TABLE, tbl)
        uc.mem_write(API_FUNCS, b"\x00\xbf" * (n_funcs * 2))   # nops (never executed)
        uc.hook_add(UC_HOOK_CODE, self.on_syscall, begin=API_FUNCS,
                    end=API_FUNCS + n_funcs * 4)

        # console state
        self.height = 180 if vmode == 1 else 240
        self.palette = np.zeros(256, dtype=np.uint16)
        self.sheets = []
        self.dlist = []
        self.clear_pending = None
        self.frame = 0
        self.pad_buttons = 0
        self.pcg_state = 0
        self.running = True
        self.save_dir = pathlib.Path("saves") / self.game_id
        self.screen = None
        if not args.headless:
            import pygame
            self.pygame = pygame
            pygame.init()
            s = args.scale
            self.screen = pygame.display.set_mode((320 * s, 240 * s))
            pygame.display.set_caption(f"sync32: {self.title}")
        self.next_frame_t = time.perf_counter()

    # ---- helpers ----
    def reg(self, r): return self.uc.reg_read(r)
    def args4(self):
        return [self.reg(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1,
                                      UC_ARM_REG_R2, UC_ARM_REG_R3)]
    def stack_args(self, n):
        sp = self.reg(UC_ARM_REG_SP)
        return list(struct.unpack(f"<{n}I", self.uc.mem_read(sp, 4 * n)))
    def ret(self, val=None):
        if val is not None:
            self.uc.reg_write(UC_ARM_REG_R0, val & 0xffffffff)
        self.uc.reg_write(UC_ARM_REG_PC, self.reg(UC_ARM_REG_LR))

    def fb(self):
        return np.frombuffer(self.uc.mem_read(FB_ADDR, 320 * 240),
                             dtype=np.uint8).reshape(240, 320).copy()

    # ---- syscalls (order MUST match sync32_api_t) ----
    def on_syscall(self, uc, addr, size, user):
        idx = (addr - API_FUNCS) // 4
        a = self.args4()
        s = self
        if idx == 0:      # exit
            s.running = False; uc.emu_stop(); return
        elif idx == 1:    # ticks_us
            s.ret(s.frame * 16667)
        elif idx == 2:    # random
            s.ret(np.random.randint(0, 1 << 32, dtype=np.uint64))
        elif idx == 3:    # rng_seed(u64 in r0:r1)
            seed = a[0] | (a[1] << 32)
            M, I = 6364136223846793005, 1442695040888963407
            st = (0 * M + I) % (1 << 64)
            st = (st + seed) % (1 << 64)
            s.pcg_state = (st * M + I) % (1 << 64)
            s.ret()
        elif idx == 4:    # rng_next
            M, I = 6364136223846793005, 1442695040888963407
            old = s.pcg_state
            s.pcg_state = (old * M + I) % (1 << 64)
            x = (((old >> 18) ^ old) >> 27) & 0xffffffff
            r = old >> 59
            s.ret(((x >> r) | (x << ((32 - r) & 31))) & 0xffffffff)
        elif idx == 5:    # palette_set(ptr)
            s.palette = np.frombuffer(uc.mem_read(a[0], 512), dtype=np.uint16).copy()
            s.ret()
        elif idx == 6:    # sheet_load(ptr, w, h)
            w, h = a[1], a[2]
            data = np.frombuffer(uc.mem_read(a[0], w * h), dtype=np.uint8).reshape(h, w).copy()
            s.sheets.append(data)
            s.ret(len(s.sheets) - 1)
        elif idx == 7:    # clear(rgb565)
            s.clear_pending = a[0] & 0xffff; s.ret()
        elif idx == 8:    # sprite(sheet, sx, sy, w, h | x, y, flags on stack)
            st = s.stack_args(4)
            s.dlist.append(("spr", a[0], a[1], a[2], a[3], st[0], st[1], st[2], st[3]))
            s.ret()
        elif idx == 9:    # rect(x, y, w, h, color-on-stack)
            st = s.stack_args(1)
            s.dlist.append(("rect", a[0], a[1], a[2], a[3], st[0]))
            s.ret()
        elif idx == 10:   # canvas()
            s.ret(FB_ADDR)
        elif idx == 11:   # canvas_mark
            s.ret()
        elif idx == 12:   # present
            s.present(); s.ret()
        elif idx == 13:   # pad(player, out*)
            buttons = s.pad_buttons if a[0] == 0 else 0
            conn = 1 if a[0] == 0 else 0
            uc.mem_write(a[1], struct.pack("<Hbbbb BB", buttons, 0, 0, 0, 0, conn, 0))
            s.ret()
        elif idx == 14:   # audio_space
            s.ret(4096)
        elif idx == 15:   # audio_push
            s.ret()
        elif idx == 16:   # save_read(slot, buf, max)
            p = s.save_dir / f"slot{a[0]}.bin"
            if 0 <= a[0] < 8 and p.exists():
                d = p.read_bytes()[:a[2]]
                uc.mem_write(a[1], d); s.ret(len(d))
            else: s.ret(-1)
        elif idx == 17:   # save_write(slot, buf, len)
            if 0 <= a[0] < 8 and a[2] <= 65536:
                s.save_dir.mkdir(parents=True, exist_ok=True)
                (s.save_dir / f"slot{a[0]}.bin").write_bytes(bytes(uc.mem_read(a[1], a[2])))
                s.ret(a[2])
            else: s.ret(-1)
        else:
            s.ret(0)

    # ---- video ----
    @staticmethod
    def to_int(v, bits=16):
        v &= (1 << bits) - 1
        return v - (1 << bits) if v >= (1 << (bits - 1)) else v

    def nearest_idx(self, c):
        p = self.palette.astype(np.int32)
        dr = ((p >> 11) & 31) - ((c >> 11) & 31)
        dg = ((p >> 5) & 63) - ((c >> 5) & 63)
        db = (p & 31) - (c & 31)
        return int(np.argmin(dr * dr * 2 + dg * dg + db * db * 2))

    def present(self):
        fbmem = np.frombuffer(bytearray(self.uc.mem_read(FB_ADDR, 320 * 240)),
                              dtype=np.uint8).reshape(240, 320)
        if self.clear_pending is not None:
            fbmem[:] = self.nearest_idx(self.clear_pending)
            self.clear_pending = None
        for op in self.dlist:
            if op[0] == "rect":
                _, x, y, w, h, c = op
                x, y = self.to_int(x, 32), self.to_int(y, 32)
                x0, y0 = max(0, x), max(0, y)
                x1, y1 = min(320, x + w), min(240, y + h)
                if x0 < x1 and y0 < y1:
                    fbmem[y0:y1, x0:x1] = self.nearest_idx(c & 0xffff)
            else:
                _, sh, sx, sy, w, h, x, y, flags = op
                if not (0 <= sh < len(self.sheets)): continue
                x, y = self.to_int(x, 32), self.to_int(y, 32)
                src = self.sheets[sh][sy:sy + h, sx:sx + w]
                if flags & 1: src = src[:, ::-1]
                if flags & 2: src = src[::-1, :]
                x0, y0 = max(0, x), max(0, y)
                x1, y1 = min(320, x + src.shape[1]), min(240, y + src.shape[0])
                if x0 >= x1 or y0 >= y1: continue
                sub = src[y0 - y:y1 - y, x0 - x:x1 - x]
                dst = fbmem[y0:y1, x0:x1]
                mask = sub != 0
                dst[mask] = sub[mask]
        self.dlist = []
        self.uc.mem_write(FB_ADDR, fbmem.tobytes())
        self.frame += 1
        self.blit(fbmem)
        if not self.args.turbo:
            self.next_frame_t += 1 / 60
            dt = self.next_frame_t - time.perf_counter()
            if dt > 0: time.sleep(dt)
            else: self.next_frame_t = time.perf_counter()
        if self.args.frames and self.frame >= self.args.frames:
            if self.args.screenshot: self.save_png(fbmem, self.args.screenshot)
            self.running = False
            self.uc.emu_stop()

    def rgb(self, fbmem):
        pal = self.palette.astype(np.uint32)
        r = ((pal >> 11) & 31) * 255 // 31
        g = ((pal >> 5) & 63) * 255 // 63
        b = (pal & 31) * 255 // 31
        lut = (r << 16) | (g << 8) | b
        return lut[fbmem]

    def blit(self, fbmem):
        if self.screen is None: return
        pg = self.pygame
        for ev in pg.event.get():
            if ev.type == pg.QUIT: self.running = False; self.uc.emu_stop()
        k = pg.key.get_pressed()
        b = 0
        if k[pg.K_UP]: b |= 0x0001
        if k[pg.K_DOWN]: b |= 0x0002
        if k[pg.K_LEFT]: b |= 0x0004
        if k[pg.K_RIGHT]: b |= 0x0008
        if k[pg.K_RETURN]: b |= 0x0010
        if k[pg.K_RSHIFT]: b |= 0x0020
        if k[pg.K_q]: b |= 0x0100
        if k[pg.K_w]: b |= 0x0200
        if k[pg.K_z]: b |= 0x1000
        if k[pg.K_x]: b |= 0x2000
        if k[pg.K_a]: b |= 0x4000
        if k[pg.K_s]: b |= 0x8000
        if k[pg.K_ESCAPE]: self.running = False; self.uc.emu_stop()
        self.pad_buttons = b
        surf = pg.surfarray.make_surface(
            np.transpose(self.rgb(fbmem).view(np.uint8).reshape(240, 320, 4)[:, :, :3][:, :, ::-1],
                         (1, 0, 2)))
        pg.transform.scale(surf, self.screen.get_size(), self.screen)
        pg.display.flip()

    def save_png(self, fbmem, path):
        rgbv = self.rgb(fbmem)
        try:
            from PIL import Image
            arr = np.zeros((240, 320, 3), dtype=np.uint8)
            arr[:, :, 0] = (rgbv >> 16) & 255
            arr[:, :, 1] = (rgbv >> 8) & 255
            arr[:, :, 2] = rgbv & 255
            Image.fromarray(arr).save(path)
            print("screenshot:", path)
        except ImportError:
            print("PIL missing; no screenshot")

    def run(self):
        self.uc.reg_write(UC_ARM_REG_SP, GAME_STACK_TOP)
        self.uc.reg_write(UC_ARM_REG_R0, API_TABLE)
        self.uc.reg_write(UC_ARM_REG_LR, (API_FUNCS) | 1)  # returning = exit
        print(f"sync32emu: '{self.title}' entry={self.entry:#x} mode={'180p' if self.video_mode else '240p'}")
        while self.running:
            try:
                self.uc.emu_start(self.entry | 1, 0, count=0)
                break
            except Exception as e:
                print("emulation stopped:", e)
                break
        print(f"exited after {self.frame} frames")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--frames", type=int, default=0)
    ap.add_argument("--screenshot")
    ap.add_argument("--headless", action="store_true")
    ap.add_argument("--turbo", action="store_true")
    a = ap.parse_args()
    if a.headless:
        import os
        os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
    Sync32Emu(a.rom, a).run()
