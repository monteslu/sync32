// sync32 MSC: expose the SD card as a USB drive (thumb-drive mode)
#include <string.h>
#include "tusb.h"
#include "hw_config.h"
#include "diskio.h"

static bool ejected = false;
volatile bool s32_msc_mode = false;   // MSC exposes media ONLY in drive mode
volatile bool s32_msc_ejected_flag = false;
volatile uint32_t s32_msc_writes = 0;

void s32_msc_reset_eject(void) { ejected = false; s32_msc_ejected_flag = false; }

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id, "sync32  ", 8);
    memcpy(product_id, "SD card         ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    if (!s32_msc_mode || ejected) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00);
        return false;
    }
    if (disk_status(0) & STA_NOINIT)
        disk_initialize(0);                 // wake the card for MSC service
    return (disk_status(0) & STA_NOINIT) == 0;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    DWORD n = 0;
    disk_ioctl(0, GET_SECTOR_COUNT, &n);
    *block_count = n;
    *block_size = 512;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun; (void)power_condition;
    if (load_eject && !start) { ejected = true; s32_msc_ejected_flag = true; }
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)lun; (void)offset;
    if (!s32_msc_mode) return -1;
    if (disk_read(0, buffer, lba, bufsize / 512) != RES_OK) return -1;
    return (int32_t)bufsize;
}

bool tud_msc_is_writable_cb(uint8_t lun) { (void)lun; return true; }

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)lun; (void)offset;
    if (!s32_msc_mode) return -1;
    if (disk_write(0, buffer, lba, bufsize / 512) != RES_OK) return -1;
    s32_msc_writes++;
    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void)lun; (void)buffer; (void)bufsize;
    switch (scsi_cmd[0]) {
        case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL: return 0;
        default:
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
            return -1;
    }
}
