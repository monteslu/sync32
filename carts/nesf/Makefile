# sync32 NES cart powered by libretro-fceumm (pinned revision), XIP mode.
SDK := ../../sdk
NAME := nesf
TITLE := NES fceumm
CORE := fceumm/src
CC := arm-none-eabi-gcc

BOARD_EXCL := $(CORE)/boards/unrom512.c $(CORE)/boards/355.c \
  $(CORE)/boards/jyasic.c $(CORE)/boards/emu2413.c
CORE_C := $(filter-out $(BOARD_EXCL),$(wildcard $(CORE)/boards/*.c)) $(wildcard $(CORE)/input/*.c) \
  $(CORE)/cart.c $(CORE)/cheat.c $(CORE)/crc32.c $(CORE)/fceu-endian.c \
  $(CORE)/fceu-memory.c $(CORE)/fceu.c $(CORE)/fds.c $(CORE)/fds_apu.c \
  $(CORE)/file.c $(CORE)/filter.c $(CORE)/general.c $(CORE)/input.c \
  $(CORE)/md5.c $(CORE)/nsf.c $(CORE)/palette.c $(CORE)/ppu.c \
  $(CORE)/sound.c $(CORE)/state.c $(CORE)/video.c $(CORE)/vsuni.c \
  $(CORE)/ines.c $(CORE)/unif.c $(CORE)/x6502.c \
  $(CORE)/drivers/libretro/libretro-common/streams/memory_stream.c \
  $(CORE)/drivers/libretro/libretro-common/file/file_path.c \
  $(CORE)/drivers/libretro/libretro-common/compat/compat_strl.c \
  $(CORE)/drivers/libretro/libretro-common/string/stdstring.c \
  $(CORE)/drivers/libretro/libretro-common/encodings/encoding_utf.c

SRCS := main.c shims.c fs_stub.c board_stubs.c $(CORE_C)

CFLAGS := -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 \
  -O2 -ffunction-sections -fdata-sections -fsingle-precision-constant \
  -Wall -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable \
  -I$(SDK)/include -I$(CORE) -I$(CORE)/input -I$(CORE)/boards \
  -I$(CORE)/drivers/libretro -I$(CORE)/drivers/libretro/libretro-common/include \
  -D__LIBRETRO__ -DPATH_MAX=1024 -DFCEU_VERSION_NUMERIC=9813 \
  -DFRONTEND_SUPPORTS_RGB565 \
  -DPRId64='"lld"' -DPRIu64='"llu"' -DPRIx64='"llx"' -DPRIi64='"lli"' \
  -nostartfiles
LDFLAGS := -Wl,--gc-sections -Wl,-T$(SDK)/crt0/xip.ld -nostartfiles

$(NAME).s32: $(NAME).elf
	$(SDK)/tools/mks32.py $< $@ --title "$(TITLE)" --id $(NAME)000 --mode xip --api 2
	arm-none-eabi-size $(NAME).elf

$(NAME).elf: $(SRCS) $(SDK)/crt0/crt0.S $(SDK)/include/sync32.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(SDK)/crt0/crt0.S $(SRCS) -lm -o $@

clean:
	rm -f $(NAME).elf $(NAME).s32
