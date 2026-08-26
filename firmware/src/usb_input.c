// sync32 USB pad input: native-controller HOST mode (xinput + hid stubs).
// The console boots in device-probe mode; main.c flips to host mode when no
// PC enumerates. All state here is inert unless s32_usb_host_start() ran.
#include <string.h>
#include "pico/stdlib.h"
#include "tusb.h"
#include "xinput_host.h"
#include "sync32.h"
#include "log.h"

extern volatile s32_pad_t s32_pads[4];

static bool host_active;
static uint32_t last_mount_ms;          // for the launcher's idle re-probe
static int mounted_pads;

// dev_addr+instance -> player slot
static struct { uint8_t addr, inst; bool used; } slot[4];

static int slot_find(uint8_t addr, uint8_t inst) {
    for (int i = 0; i < 4; i++)
        if (slot[i].used && slot[i].addr == addr && slot[i].inst == inst) return i;
    return -1;
}
static int slot_alloc(uint8_t addr, uint8_t inst) {
    for (int i = 0; i < 4; i++)
        if (!slot[i].used) { slot[i] = (typeof(slot[0])){addr, inst, true}; return i; }
    return -1;
}

usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &usbh_xinput_driver;
}

// device-level: fires for EVERY successfully enumerated device, before any
// class driver claims it. If a device attaches but this never logs, the
// failure is inside enumeration itself.
void tuh_mount_cb(uint8_t dev_addr) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    s32_log("usb: enumerated addr=%u vid=%04x pid=%04x", dev_addr, vid, pid);
}
void tuh_umount_cb(uint8_t dev_addr) {
    s32_log("usb: device %u gone", dev_addr);
}

void tuh_xinput_mount_cb(uint8_t dev_addr, uint8_t instance, const xinputh_interface_t *itf) {
    s32_log("usb: xinput mount addr=%u type=%d", dev_addr, itf->type);
    if (itf->type == XBOX360_WIRED) tuh_xinput_set_led(dev_addr, instance, 1, true);
    tuh_xinput_receive_report(dev_addr, instance);
    if (slot_alloc(dev_addr, instance) >= 0) mounted_pads++;
    last_mount_ms = to_ms_since_boot(get_absolute_time());
}

void tuh_xinput_umount_cb(uint8_t dev_addr, uint8_t instance) {
    int i = slot_find(dev_addr, instance);
    if (i >= 0) {
        slot[i].used = false;
        memset((void *)&s32_pads[i], 0, sizeof s32_pads[0]);
        mounted_pads--;
        last_mount_ms = to_ms_since_boot(get_absolute_time());
    }
}

void tuh_xinput_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                   xinputh_interface_t const *itf, uint16_t len) {
    (void)len;
    int i = slot_find(dev_addr, instance);
    if (i >= 0 && itf->connected && itf->new_pad_data) {
        s32_pads[i].buttons = itf->pad.wButtons;
        s32_pads[i].lx = (int8_t)(itf->pad.sThumbLX >> 8);
        s32_pads[i].ly = (int8_t)(itf->pad.sThumbLY >> 8);
    }
    tuh_xinput_receive_report(dev_addr, instance);
}

// generic HID pads: input decoding is future work, but their PRESENCE must
// count: a HID-only pad (8BitDo in DInput/Switch mode) used to leave
// mounted_pads at 0 and the launcher's 15s idle re-probe bootlooped forever
static int mounted_hids;
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)report; (void)len;
    s32_log("usb: hid mount addr=%u itf=%u rdesc=%u", dev_addr, instance, len);
    mounted_hids++;
    last_mount_ms = to_ms_since_boot(get_absolute_time());
    tuh_hid_receive_report(dev_addr, instance);
}
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    (void)dev_addr; (void)instance;
    if (mounted_hids > 0) mounted_hids--;
    last_mount_ms = to_ms_since_boot(get_absolute_time());
}
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)report; (void)len;
    tuh_hid_receive_report(dev_addr, instance);
}

void s32_usb_host_start(void) {
    tuh_init(0);                 // native USB controller as host
    host_active = true;
    last_mount_ms = to_ms_since_boot(get_absolute_time());
}

#include "hardware/structs/usb.h"
// something physically on the port (even if enumeration failed): SIE speed
// bits are nonzero while a device is attached. The launcher must NOT
// self-heal-bounce in that case: an unenumerable pad (8BitDo Ultimate C)
// made the 15s bounce reboot the console forever.
bool s32_usb_device_attached(void) {
    return (usb_hw->sie_status & USB_SIE_STATUS_SPEED_BITS) != 0;
}

bool s32_usb_host_active(void) { return host_active; }
int  s32_usb_pads_mounted(void) { return mounted_pads; }
int  s32_usb_hids_mounted(void) { return mounted_hids; }
int  s32_usb_devs_mounted(void) { return mounted_pads + mounted_hids; }
uint32_t s32_usb_last_mount_ms(void) { return last_mount_ms; }

// pumped once per presented frame (launcher AND games call present)
void s32_usb_task(void) {
    if (!host_active) return;
    static bool was_attached;
    bool att = s32_usb_device_attached();
    if (att != was_attached) {
        s32_log("usb: port %s (sie=%08lx)", att ? "ATTACH" : "detach",
                (unsigned long)usb_hw->sie_status);
        was_attached = att;
    }
    tuh_task();
}
