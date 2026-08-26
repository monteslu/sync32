// sync32 file drop: get files onto the SD without thumb-drive mode.
// One sink, two transports:
//   1) SWD mailbox: the host (openocd via debugprobe) writes chunks into a
//      control block + game-RAM buffer; the launcher polls it every frame.
//   2) YMODEM over stdio (USB-CDC or UART): 'r' at the menu, then sz from
//      any serial terminal. Works for a plain Pico + usb-serial dongle.
// Both run ONLY from the launcher loop, so they never fight a game for SD.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "pico/stdio_usb.h"
#include "ff.h"
#include "s32crc.h"
#include "video.h"

extern volatile bool s32_long_op;
int sd_mount(void);

// ---- sink: SD file being received (temp + atomic rename) ----
static FIL xf;
static bool xf_open_flag;
static char xf_final[96], xf_part[100];

static bool name_ok(const char *n) {
    // plain name or one subdir level; no dotdot, no leading slash
    if (!n || !n[0] || n[0] == '/' || strstr(n, "..")) return false;
    int slashes = 0;
    for (const char *p = n; *p; p++) {
        if (*p == '\\' || *p == ':') return false;
        if (*p == '/') slashes++;
    }
    return slashes <= 1 && strlen(n) < sizeof xf_final;
}

static int sink_open(const char *name) {
    if (xf_open_flag) { f_close(&xf); xf_open_flag = false; }
    if (!name_ok(name) || sd_mount() != 0) return -1;
    strcpy(xf_final, name);
    const char *slash = strchr(name, '/');
    if (slash) {                       // ensure the one subdir exists
        char dir[96];
        memcpy(dir, name, slash - name);
        dir[slash - name] = 0;
        f_mkdir(dir);                  // EEXIST is fine
    }
    snprintf(xf_part, sizeof xf_part, "%s.part", name);
    s32_long_op = true;
    FRESULT fr = f_open(&xf, xf_part, FA_WRITE | FA_CREATE_ALWAYS);
    s32_long_op = false;
    if (fr != FR_OK) return -2;
    xf_open_flag = true;
    return 0;
}

static int sink_write(const void *p, unsigned n) {
    if (!xf_open_flag) return -1;
    UINT bw;
    s32_long_op = true;
    FRESULT fr = f_write(&xf, p, n, &bw);
    s32_long_op = false;
    return (fr == FR_OK && bw == n) ? 0 : -2;
}

static int sink_close(void) {
    if (!xf_open_flag) return -1;
    xf_open_flag = false;
    s32_long_op = true;
    f_close(&xf);
    f_unlink(xf_final);
    FRESULT fr = f_rename(xf_part, xf_final);
    s32_long_op = false;
    return fr == FR_OK ? 0 : -2;
}

static void sink_abort(void) {
    if (!xf_open_flag) return;
    xf_open_flag = false;
    f_close(&xf);
    f_unlink(xf_part);
}

// ---- transport 1: SWD mailbox ----
// Host finds s32_xfer_mbox via nm, data buffer is game RAM (free while the
// launcher runs). Protocol: host fills fields, sets cmd; firmware executes,
// writes status, clears cmd. crc32 (IEEE) guards every data chunk.
#define MBOX_MAGIC 0x53325846u          // "FX2S" little-endian
#define MBOX_BUF   ((uint8_t *)0x20068000)
#define MBOX_BUF_MAX 16384
enum { MB_IDLE = 0, MB_OPEN = 1, MB_DATA = 2, MB_CLOSE = 3, MB_ABORT = 4 };

typedef struct {
    uint32_t magic;
    volatile uint32_t cmd;
    volatile int32_t status;            // result of last command
    volatile uint32_t len;              // data bytes in buffer
    volatile uint32_t crc32;            // of buffer[0..len)
    char name[64];                      // for MB_OPEN
} s32_xfer_mbox_t;

s32_xfer_mbox_t s32_xfer_mbox = { .magic = MBOX_MAGIC };

static uint32_t mbox_poll_once(void) {  // returns the command handled
    uint32_t c = s32_xfer_mbox.cmd;
    if (c == MB_IDLE) return MB_IDLE;
    int st = -100;
    switch (c) {
    case MB_OPEN:
        s32_xfer_mbox.name[sizeof s32_xfer_mbox.name - 1] = 0;
        st = sink_open(s32_xfer_mbox.name);
        break;
    case MB_DATA: {
        uint32_t n = s32_xfer_mbox.len;
        if (n > MBOX_BUF_MAX) st = -3;
        else if (s32_crc32(MBOX_BUF, n) != s32_xfer_mbox.crc32) st = -4;
        else st = sink_write(MBOX_BUF, n);
        break;
    }
    case MB_CLOSE: st = sink_close(); break;
    case MB_ABORT: sink_abort(); st = 0; break;
    }
    s32_xfer_mbox.status = st;
    __asm volatile("dmb" ::: "memory");
    s32_xfer_mbox.cmd = MB_IDLE;        // last: host sees status valid
    return c;
}

int s32_xfer_poll(void) {               // launcher calls once per frame;
    int file_done = 0;                  // returns 1 when a file completed
    uint32_t c;
    while ((c = mbox_poll_once()) != MB_IDLE)
        if (c == MB_CLOSE && s32_xfer_mbox.status == 0) file_done = 1;
    return file_done;
}

// ---- transport 2: YMODEM receive (CRC mode, 128B + 1K blocks, batch) ----
#define YBUF ((uint8_t *)0x2006D000)    // game RAM: 1K block + header room
#define SOH 0x01
#define STX 0x02
#define EOT 0x04
#define ACK 0x06
#define NAK 0x15
#define CAN 0x18

static uint16_t crc16_ccitt(const uint8_t *p, int n) {
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)*p++ << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

static int ygetc(uint32_t timeout_ms) {
    return getchar_timeout_us(timeout_ms * 1000);
}

static void yprogress(uint32_t got, uint32_t total, int files) {
    video_clear(0x0000);
    video_rect(20, 100, 280, 14, 0x8410);
    if (total) {
        uint32_t w = (uint64_t)280 * (got > total ? total : got) / total;
        video_rect(20, 100, (int)w, 14, 0x07E0);
    }
    for (int i = 0; i < files; i++) video_rect(20 + i * 14, 130, 8, 8, 0xFFFF);
    video_present();                    // also feeds the watchdog path
}

// receive one block into YBUF; returns block size (128/1024), 0=EOT,
// -1=timeout/garbage, -2=cancel. seq_out gets the block number.
static int yblock(uint8_t *seq_out) {
    int c = ygetc(1500);
    if (c == PICO_ERROR_TIMEOUT) return -1;
    if (c == EOT) return 0;
    if (c == CAN) { if (ygetc(500) == CAN) return -2; return -1; }
    int size = c == SOH ? 128 : c == STX ? 1024 : -1;
    if (size < 0) return -1;
    int seq = ygetc(500), nseq = ygetc(500);
    if (seq < 0 || nseq < 0 || (seq ^ nseq) != 0xFF) return -1;
    for (int i = 0; i < size + 2; i++) {
        c = ygetc(500);
        if (c < 0) return -1;
        YBUF[i] = (uint8_t)c;
    }
    if (crc16_ccitt(YBUF, size) != ((YBUF[size] << 8) | YBUF[size + 1]))
        return -1;
    *seq_out = (uint8_t)seq;
    return size;
}

// full batch receive; returns files received or <0
int s32_ymodem_rx(void) {
    // binary-safe stdio: no CRLF translation while the protocol runs
    stdio_set_translate_crlf(&stdio_usb, false);
    int files = 0;
    while (1) {                          // one file per pass
        uint32_t fsize = 0, got = 0;
        uint8_t expect = 1, seq;
        int tries = 0, started = 0, size;
        while (1) {                      // wait for block 0
            putchar_raw('C');
            size = yblock(&seq);
            if (size > 0 && seq == 0) break;
            if (size == -2 || ++tries > 15) goto out;
        }
        if (YBUF[0] == 0) {              // empty name = end of batch
            putchar_raw(ACK);
            break;
        }
        char name[64];
        strncpy(name, (char *)YBUF, sizeof name - 1);
        name[sizeof name - 1] = 0;
        fsize = (uint32_t)strtoul((char *)YBUF + strlen(name) + 1, NULL, 10);
        if (sink_open(name) != 0) { putchar_raw(CAN); putchar_raw(CAN); goto out; }
        putchar_raw(ACK);
        putchar_raw('C');
        tries = 0;
        while (1) {                      // data blocks
            size = yblock(&seq);
            if (size > 0) {
                tries = 0;
                if (seq == (uint8_t)(expect - 1)) { putchar_raw(ACK); continue; }
                if (seq != expect) { sink_abort(); goto out; }
                uint32_t take = size;
                if (fsize && got + take > fsize) take = fsize - got;
                if (sink_write(YBUF, take) != 0) { sink_abort(); goto out; }
                got += take;
                expect++;
                putchar_raw(ACK);
                if ((expect & 7) == 0) yprogress(got, fsize, files);
            } else if (size == 0) {      // EOT: NAK once, ACK second
                putchar_raw(NAK);
                if (ygetc(2000) == EOT) putchar_raw(ACK);
                if (sink_close() != 0) goto out;
                files++;
                yprogress(got, fsize ? fsize : 1, files);
                if (!started) started = 1;
                break;                   // next file's block 0
            } else if (size == -2 || ++tries > 15) {
                sink_abort();
                goto out;
            } else {
                putchar_raw(NAK);
            }
        }
    }
out:
    sink_abort();                        // no-op if cleanly closed
    stdio_set_translate_crlf(&stdio_usb, true);
    return files;
}
