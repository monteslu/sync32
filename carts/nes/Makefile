MODE ?= ram
SDK := ../../sdk
NAME ?= nes
TITLE ?= NES
CC := arm-none-eabi-gcc
CFLAGS := -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 \
  -O2 -ffunction-sections -fdata-sections -fsingle-precision-constant \
  -Wall -I$(SDK)/include -nostartfiles -ffreestanding
LDFLAGS := -Wl,--gc-sections -Wl,-T$(SDK)/crt0/$(MODE).ld -nostartfiles

$(NAME).s32: $(NAME).elf
	$(SDK)/tools/mks32.py $< $@ --title "$(TITLE)" --id $(NAME)0000 --mode $(MODE) --api 2

$(NAME).elf: main.c agnes.c agnes.h font8.h $(SDK)/crt0/crt0.S $(SDK)/include/sync32.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(SDK)/crt0/crt0.S main.c -o $@
	arm-none-eabi-size $@

clean:
	rm -f $(NAME).elf $(NAME).s32
