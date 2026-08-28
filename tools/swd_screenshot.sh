#!/bin/bash
# Render the console's framebuffer to a PNG over SWD.
#   tools/swd_screenshot.sh out.png [path/to/sync32.elf]
#
# Reads s32_framebuf + cur_palette straight off the running chip, so it works
# when video capture does not (the lab capture card delivers picture in only
# about 10% of frames). Addresses are resolved from the ELF every run: they
# move whenever .bss changes, and a stale hardcoded address silently renders
# a black or garbage image that looks like a console fault.
set -e
OUT=${1:?usage: swd_screenshot.sh out.png [sync32.elf]}
ELF=${2:-$(dirname "$0")/../firmware/build/sync32.elf}
OCD=${OCD:-$HOME/code/pico/openocd-install/bin/openocd}
CFG="-f interface/cmsis-dap.cfg -f target/rp2350.cfg"

FB=0x$(arm-none-eabi-nm "$ELF" | awk '$3=="s32_framebuf"{print $1}')
PAL=0x$(arm-none-eabi-nm "$ELF" | awk '$3=="cur_palette"{print $1}')
[ "$FB" = "0x" ] && { echo "no s32_framebuf in $ELF" >&2; exit 1; }

"$OCD" $CFG -c "adapter speed 4000" -c "init" \
  -c "dump_image /tmp/fb.bin $FB 76800" \
  -c "dump_image /tmp/pal.bin $PAL 512" -c "shutdown" >/dev/null 2>&1

python3 - "$OUT" <<'PY'
import struct, sys
from PIL import Image
fb = open("/tmp/fb.bin", "rb").read()
pal = struct.unpack("<256H", open("/tmp/pal.bin", "rb").read())
img = Image.new("RGB", (320, 240)); px = img.load()
for y in range(240):
    for x in range(320):
        c = pal[fb[y * 320 + x]]
        px[x, y] = (((c >> 11) & 0x1f) << 3, ((c >> 5) & 0x3f) << 2, (c & 0x1f) << 3)
img.resize((640, 480), Image.NEAREST).save(sys.argv[1])
print("nonzero:", sum(1 for v in fb if v))
PY
