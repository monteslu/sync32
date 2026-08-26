// sync32 composite USB: CDC (serial console) + picotool reset + MSC (SD as thumb drive)
// Derived from pico-sdk stdio_usb_descriptors.c (BSD-3) + tinyusb examples (MIT)
#include "tusb.h"
#include "pico/stdio_usb/reset_interface.h"
#include "pico/unique_id.h"
#include "pico/usb_reset_interface.h"

#define USBD_VID 0x2E8A
#define USBD_PID 0x0009

#define USBD_ITF_CDC 0        // uses 2 interfaces
#define USBD_ITF_RPI_RESET 2
#define USBD_ITF_MSC 3
#define USBD_ITF_MAX 4

#define USBD_CDC_EP_CMD 0x81
#define USBD_CDC_EP_OUT 0x02
#define USBD_CDC_EP_IN  0x82
#define USBD_MSC_EP_OUT 0x03
#define USBD_MSC_EP_IN  0x83

#define USBD_CDC_CMD_MAX_SIZE 8
#define USBD_CDC_IN_OUT_MAX_SIZE 64

#define USBD_STR_0 0
#define USBD_STR_MANUF 1
#define USBD_STR_PRODUCT 2
#define USBD_STR_SERIAL 3
#define USBD_STR_CDC 4
#define USBD_STR_RPI_RESET 5
#define USBD_STR_MSC 6

#define TUD_RPI_RESET_DESC_LEN 9
#define USBD_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_RPI_RESET_DESC_LEN + TUD_MSC_DESC_LEN)

static const tusb_desc_device_t usbd_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USBD_VID,
    .idProduct = USBD_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = USBD_STR_MANUF,
    .iProduct = USBD_STR_PRODUCT,
    .iSerialNumber = USBD_STR_SERIAL,
    .bNumConfigurations = 1,
};

#define TUD_RPI_RESET_DESCRIPTOR(_itfnum, _stridx) \
  9, TUSB_DESC_INTERFACE, _itfnum, 0, 0, TUSB_CLASS_VENDOR_SPECIFIC, RESET_INTERFACE_SUBCLASS, RESET_INTERFACE_PROTOCOL, _stridx,

static const uint8_t usbd_desc_cfg[USBD_DESC_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, USBD_ITF_MAX, USBD_STR_0, USBD_DESC_LEN,
        0, 250),
    TUD_CDC_DESCRIPTOR(USBD_ITF_CDC, USBD_STR_CDC, USBD_CDC_EP_CMD,
        USBD_CDC_CMD_MAX_SIZE, USBD_CDC_EP_OUT, USBD_CDC_EP_IN, USBD_CDC_IN_OUT_MAX_SIZE),
    TUD_RPI_RESET_DESCRIPTOR(USBD_ITF_RPI_RESET, USBD_STR_RPI_RESET)
    TUD_MSC_DESCRIPTOR(USBD_ITF_MSC, USBD_STR_MSC, USBD_MSC_EP_OUT, USBD_MSC_EP_IN, 64),
};

static char usbd_serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

static const char *const usbd_desc_str[] = {
    [USBD_STR_MANUF] = "sync32",
    [USBD_STR_PRODUCT] = "sync32 console",
    [USBD_STR_SERIAL] = usbd_serial_str,
    [USBD_STR_CDC] = "sync32 console (serial)",
    [USBD_STR_RPI_RESET] = "Reset",
    [USBD_STR_MSC] = "sync32 SD card",
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&usbd_desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(__unused uint8_t index) {
    return usbd_desc_cfg;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, __unused uint16_t langid) {
    #define DESC_STR_MAX 20
    static uint16_t desc_str[DESC_STR_MAX];
    if (!usbd_serial_str[0])
        pico_get_unique_board_id_string(usbd_serial_str, sizeof usbd_serial_str);
    uint8_t len;
    if (index == 0) {
        desc_str[1] = 0x0409;
        len = 1;
    } else {
        if (index >= sizeof(usbd_desc_str) / sizeof(usbd_desc_str[0])) return NULL;
        const char *str = usbd_desc_str[index];
        for (len = 0; len < DESC_STR_MAX - 1 && str[len]; ++len)
            desc_str[1 + len] = str[len];
    }
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str;
}
