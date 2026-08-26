// sync32: SD card wiring for the Waveshare RP2350-PiZero (TF slot on spi1)
#include "hw_config.h"

static spi_t spi = {
    .hw_inst = spi1,
    .sck_gpio = 30,
    .mosi_gpio = 31,
    .miso_gpio = 40,
    .baud_rate = 20 * 1000 * 1000,
};
static sd_spi_if_t spi_if = { .spi = &spi, .ss_gpio = 43 };
static sd_card_t sd_card = { .type = SD_IF_SPI, .spi_if_p = &spi_if };

size_t sd_get_num() { return 1; }
sd_card_t *sd_get_by_num(size_t num) { return num == 0 ? &sd_card : NULL; }
