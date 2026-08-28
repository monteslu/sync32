#!/bin/bash
# Render the console's framebuffer to a PNG over SWD.
#   tools/swd_screenshot.sh out.png
# Reads s32_framebuf + cur_palette straight off the running chip, so it works
# when video capture does not -- the lab capture card here drops ~90% of frames
# and this was the only reliable way to see what the console was showing.
# Addresses are from the current build:
#   nm firmware/build/sync32.elf | grep -E 's32_framebuf|cur_palette'
OCD=~/code/pico/openocd-install/bin/openocd; CFG="-f interface/cmsis-dap.cfg -f target/rp2350.cfg"
$OCD $CFG -c "adapter speed 4000" -c "init" -c "dump_image /tmp/fb.bin 0x2000a32c 76800" -c "dump_image /tmp/pal.bin 0x20007ec8 512" -c "shutdown" >/dev/null 2>&1
python3 - "$1" <<'PY'
import struct,sys
from PIL import Image
fb=open("/tmp/fb.bin","rb").read()
pal=struct.unpack("<256H", open("/tmp/pal.bin","rb").read())
img=Image.new("RGB",(320,240)); px=img.load()
for y in range(240):
    for x in range(320):
        c=pal[fb[y*320+x]]
        px[x,y]=(((c>>11)&0x1f)<<3, ((c>>5)&0x3f)<<2, (c&0x1f)<<3)
img.resize((640,480), Image.NEAREST).save(sys.argv[1])
print("nonzero:", sum(1 for v in fb if v))
PY
