// sync32 persistent event log: a small text ring in .uninitialized_data,
// so it SURVIVES warm reboots (watchdog, SystemReset, crashes). Readable
// over SWD at any time (sdk/tools/s32log.py) and printed to stdio at boot.
// Power loss wipes it (that absence is itself evidence).
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "pico/stdlib.h"
#include "log.h"

#define LOG_MAGIC 0x53324C47u   // "GL2S"
#define LOG_SIZE  2048

static struct {
    uint32_t magic;
    uint32_t seq;               // monotonically increasing entry counter
    uint32_t head;              // next write offset into buf
    uint32_t boot;              // boot number (increments each s32_log_init)
    uint32_t last_alive;        // boot number that last proved itself healthy
    char buf[LOG_SIZE];
} ring __attribute__((section(".uninitialized_data.s32log")));

static void put(const char *s) {
    while (*s) {
        ring.buf[ring.head] = *s++;
        ring.head = (ring.head + 1) % LOG_SIZE;
    }
    ring.buf[ring.head] = 0;    // ring stays NUL-terminated at head
}

void s32_log(const char *fmt, ...) {
    char line[96];
    int n = snprintf(line, sizeof line, "[%lu.%03lu #%lu] ",
                     (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000),
                     (unsigned long)(to_ms_since_boot(get_absolute_time()) % 1000),
                     (unsigned long)ring.seq++);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - n - 2, fmt, ap);
    va_end(ap);
    size_t l = strlen(line);
    line[l] = '\n';
    line[l + 1] = 0;
    put(line);
}

void s32_log_init(void) {
    if (ring.magic != LOG_MAGIC) {      // cold power-up (or corrupted): start fresh
        memset(&ring, 0, sizeof ring);
        ring.magic = LOG_MAGIC;
        s32_log("log: cold start (power-up)");
    } else {
        ring.boot++;
        s32_log("log: === boot %lu (warm) ===", (unsigned long)ring.boot);
    }
}

uint32_t s32_log_boot_count(void) { return ring.boot; }

// rapid-death tracking lives HERE (reset-surviving RAM), not in watchdog
// scratch: scratch[4..7] belong to the bootrom boot-vector and get wiped
// by watchdog_reboot — the first quarantine attempt silently never armed.
void s32_log_mark_alive(void) { ring.last_alive = ring.boot; }
uint32_t s32_log_rapid_deaths(void) {
    return ring.boot - ring.last_alive;
}

void s32_log_dump_stdio(void) {
    // oldest-first: from just past head (NUL) to head
    printf("--- s32 log (boot %lu, seq %lu) ---\n",
           (unsigned long)ring.boot, (unsigned long)ring.seq);
    uint32_t i = (ring.head + 1) % LOG_SIZE;
    while (i != ring.head) {
        char c = ring.buf[i];
        if (c) putchar(c);
        i = (i + 1) % LOG_SIZE;
    }
    printf("--- end log ---\n");
}
