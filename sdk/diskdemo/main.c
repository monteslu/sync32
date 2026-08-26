// Disk Demo: proves api v2 streaming. Verifies a 1MB data file (3x all of
// game RAM) via sequential streaming, then random seeks, using a PCG32
// keyed per 4KB block so every byte is checkable without storing any of it.
#include "sync32.h"

#define BLOCK 4096
#define CHUNK 16384

static const sync32_api_t *ab;
static uint8_t chunk[CHUNK];

// same generator the packer uses: PCG32 seeded with the block index
static uint32_t pcg_out(uint64_t s) {
    uint32_t x = (uint32_t)(((s >> 18) ^ s) >> 27);
    uint32_t r = (uint32_t)(s >> 59);
    return (x >> r) | (x << ((32 - r) & 31));
}
static uint64_t block_seed(uint32_t block) {
    uint64_t s = 0;
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    s += 0xD15CDA7A00ULL + block;
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return s;
}
static int verify_block(uint32_t block, const uint8_t *data, int len) {
    uint64_t st = block_seed(block);
    for (int i = 0; i < len; i += 4) {
        uint64_t old = st;
        st = st * 6364136223846793005ULL + 1442695040888963407ULL;
        uint32_t v = pcg_out(old);
        for (int b = 0; b < 4 && i + b < len; b++)
            if (data[i + b] != (uint8_t)(v >> (8 * b))) return 0;
    }
    return 1;
}

#define C_WHITE 0xFFFF
#define C_GREEN 0x07E0
#define C_RED   0xF800
#define C_BLUE  0x001F
#define C_GREY  0x8410
static void bar(int x, int y, int w, int h, uint16_t c) { ab->rect(x, y, w, h, c); }

static void frame_step(void) { ab->present(); }

void game_main(const sync32_api_t *api) {
    ab = api;
    uint16_t pal[256];
    for (int i = 0; i < 256; i++) pal[i] = 0;
    pal[1] = 0xFFFF; pal[2] = 0x07E0; pal[3] = 0xF800;   // white green red
    pal[4] = 0x001F; pal[5] = 0x8410;                     // blue grey
    ab->palette_set(pal);

    if (ab->api_version < 2) {          // firmware too old for disk
        for (;;) { ab->clear(0xF800); frame_step(); }
    }

    // phase 0: enumerate our data dir: expect data.bin present
    char name[S32_DISK_NAME_MAX]; uint32_t fsize = 0;
    int found = 0;
    for (int i = 0; ; i++) {
        uint32_t sz;
        if (ab->disk_list(i, name, &sz) != S32_DISK_EOK) break;
        int is_data = 1;
        const char *want = "data.bin";
        for (int k = 0; k < 9; k++) if (name[k] != want[k]) { is_data = 0; break; }
        if (is_data) { found = 1; fsize = sz; }
    }

    int fd = found ? ab->disk_open("data.bin") : -99;
    int size = fd >= 0 ? ab->disk_size(fd) : -1;
    int ok_meta = found && fd >= 0 && (uint32_t)size == fsize && size > 0;

    uint32_t blocks = ok_meta ? (uint32_t)size / BLOCK : 0;
    uint32_t done = 0, bad = 0;
    uint32_t t0 = ab->ticks_us();

    // phase 1: sequential stream, verifying every byte, chunked per frame
    while (ok_meta && done < blocks) {
        int got = ab->disk_read(fd, chunk, CHUNK);
        if (got <= 0) { bad++; break; }
        for (int off = 0; off + BLOCK <= got; off += BLOCK, done++)
            if (!verify_block(done, chunk + off, BLOCK)) bad++;
        ab->clear(0x0000);
        bar(20, 100, 280, 14, C_GREY);
        bar(20, 100, (int)(280 * done / (blocks ? blocks : 1)), 14, bad ? C_RED : C_GREEN);
        bar(20, 130, 8, 8, C_WHITE);   // phase marker: one dot = sequential
        frame_step();
    }
    uint32_t seq_us = ab->ticks_us() - t0;

    // phase 2: 64 random seeks, verify one block each
    uint32_t rbad = 0;
    ab->rng_seed(0x5EEC);
    for (int i = 0; ok_meta && i < 64; i++) {
        uint32_t blk = ab->rng_next() % blocks;
        if (ab->disk_seek(fd, blk * BLOCK) != S32_DISK_EOK) { rbad++; continue; }
        int got = ab->disk_read(fd, chunk, BLOCK);
        if (got != BLOCK || !verify_block(blk, chunk, BLOCK)) rbad++;
        ab->clear(0x0000);
        bar(20, 100, 280, 14, C_GREY);
        bar(20, 100, 280 * (i + 1) / 64, 14, rbad ? C_RED : C_GREEN);
        bar(20, 130, 8, 8, C_WHITE); bar(34, 130, 8, 8, C_WHITE);   // two dots = random
        frame_step();
    }
    if (fd >= 0) ab->disk_close(fd);

    // sanity: errors must actually error
    int neg = 0;
    if (ab->disk_open("../escape") != S32_DISK_EINVAL) neg++;
    if (ab->disk_open("missing.bin") != S32_DISK_ENOENT) neg++;
    if (ab->disk_read(fd, chunk, 16) != S32_DISK_EBADF) neg++;  // fd is closed

    int pass = ok_meta && !bad && !rbad && !neg && done == blocks;

    // result screen: green = pass, red = fail; throughput as a bar row
    // (KB/s / 8) pixels wide, grey ruler = 1MB/s
    uint32_t kbs = seq_us ? (uint32_t)((uint64_t)done * BLOCK * 1000000ULL
                                       / seq_us / 1024ULL) : 0;
    for (;;) {
        ab->clear(0x0000);
        bar(0, 0, 320, 30, pass ? C_GREEN : C_RED);
        bar(20, 60, 8, 8, ok_meta ? C_GREEN : C_RED);       // meta ok
        bar(34, 60, 8, 8, bad == 0 ? C_GREEN : C_RED);      // sequential clean
        bar(48, 60, 8, 8, rbad == 0 ? C_GREEN : C_RED);     // random clean
        bar(62, 60, 8, 8, neg == 0 ? C_GREEN : C_RED);      // error paths correct
        bar(20, 90, 125, 4, C_GREY);              // 1MB/s ruler
        bar(20, 84, (int)(kbs / 8) > 300 ? 300 : (int)(kbs / 8), 4, C_BLUE);
        frame_step();
    }
}
