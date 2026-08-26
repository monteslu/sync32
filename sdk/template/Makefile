# sync32 game template. make MODE=ram|xip
MODE ?= ram
SDK := ..
NAME ?= template
TITLE ?= Template
CC := arm-none-eabi-gcc
CFLAGS := -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 \
  -O2 -ffunction-sections -fdata-sections -fsingle-precision-constant \
  -Wall -I$(SDK)/include -nostartfiles -ffreestanding
LDFLAGS := -Wl,--gc-sections -Wl,-T$(SDK)/crt0/$(MODE).ld -nostartfiles

$(NAME).s32: $(NAME).elf
	$(SDK)/tools/mks32.py $< $@ --title "$(TITLE)" --id $(NAME)00 --mode $(MODE)

$(NAME).elf: main.c $(SDK)/crt0/crt0.S $(SDK)/include/sync32.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(SDK)/crt0/crt0.S main.c -o $@

clean:
	rm -f $(NAME).elf $(NAME).s32
