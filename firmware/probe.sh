#!/bin/bash
# sync32 probe helper: dump named symbols via the debugprobe
# usage: probe.sh sym1 sym2 ...   (or: probe.sh --flash)
ELF=~/code/sync32/firmware/build/sync32.elf
OCD=~/code/pico/openocd-install/bin/openocd
SC=~/code/pico/openocd-install/share/openocd/scripts
if [ "$1" = "--flash" ]; then
  exec timeout 90 $OCD -s $SC -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
    -c "adapter speed 5000" -c "program $ELF verify reset exit" 2>&1 | tail -3
fi
CMDS="init; halt"
for sym in "$@"; do
  ADDR=$(arm-none-eabi-nm $ELF | awk -v s="$sym" '$3==s {print "0x"$1}' | head -1)
  [ -z "$ADDR" ] && { echo "no symbol: $sym"; continue; }
  CMDS="$CMDS; echo $sym@$ADDR:; mdw $ADDR 8"
done
CMDS="$CMDS; resume; exit"
timeout 30 $OCD -s $SC -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "adapter speed 5000" -c "$CMDS" 2>&1 | grep -E '@0x|^0x'
