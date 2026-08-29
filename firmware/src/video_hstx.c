// HSTX DVI backend: 640x480p60 through the RP2350's hardware TMDS encoder.
//
// STATUS: written 2026-08-29, NEVER RUN ON HARDWARE. It builds and the
// register programming follows the datasheet, but no display has confirmed
// it. The pin mapping in particular assumes an Adafruit-style HSTX-to-DVI
// breakout; a different board wires the lanes in a different order and the
// symptom is a picture with wrong colours or no lock at all.
//
// What this buys over the PIO-DVI backend, honestly:
//
//   It does NOT give games more CPU. Scanout runs on core 1 in every backend
//   and the ABI's 2.5M cycles/frame is already core 0's budget with video
//   paid for. The emulator carts are 159-493% over that budget entirely on
//   their own account, and no video backend changes it.
//
//   What it DOES give is PIO. Bit-banging TMDS costs three state machines and
//   a PIO block; HSTX costs none, because the TMDS encoder is in silicon.
//   That frees PIO for USB host ports (player 2+), SDIO, and retro controller
//   ports. On a console that is the scarce resource.
//
// HSTX is fixed to GPIO12-19. That is hardware, not a choice, which is why
// this is a separate build rather than a runtime option.
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "video_backend.h"

// ---- timing: the same 640x480p60 the PIO-DVI backend emits ---------------
#define H_ACTIVE 640
#define H_FRONT   16
#define H_SYNC    96
#define H_BACK    48
#define V_ACTIVE 480
#define V_FRONT   10
#define V_SYNC     2
#define V_BACK    33
#define V_TOTAL  (V_ACTIVE + V_FRONT + V_SYNC + V_BACK)

// TMDS control symbols for the blanking periods. These are the four fixed
// 10-bit codes the standard defines for the two sync bits; they are not
// encoded data and the expander must not touch them.
#define SYNC_V0_H0 0x354u
#define SYNC_V0_H1 0x0abu
#define SYNC_V1_H0 0x154u
#define SYNC_V1_H1 0x2abu

// 640x480p60 is negative-sync on both axes, so the IDLE level outside a pulse
// is 1 and the pulse itself is 0. Getting this inverted is a display that
// says "no signal" while the pixel clock is perfectly correct.
#define CTRL_ACTIVE   SYNC_V1_H1
#define CTRL_HSYNC    SYNC_V1_H0
#define CTRL_VSYNC    SYNC_V0_H1
#define CTRL_VHSYNC   SYNC_V0_H0

// ---- command lists -------------------------------------------------------
// HSTX consumes a stream of 32-bit words. Raw words go out as-is (used for
// the control-symbol runs); the TMDS expander turns pixel words into encoded
// symbols on the fly, which is the whole point of this peripheral.
#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// One line of blanking, and the vertical-sync variants. Each is a short
// command list the DMA replays; only the active-video line changes per row.
static uint32_t vblank_line[] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT, CTRL_ACTIVE,
    HSTX_CMD_RAW_REPEAT | H_SYNC,  CTRL_HSYNC,
    HSTX_CMD_RAW_REPEAT | (H_BACK + H_ACTIVE), CTRL_ACTIVE,
    HSTX_CMD_NOP
};
static uint32_t vsync_line[] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT, CTRL_VSYNC,
    HSTX_CMD_RAW_REPEAT | H_SYNC,  CTRL_VHSYNC,
    HSTX_CMD_RAW_REPEAT | (H_BACK + H_ACTIVE), CTRL_VSYNC,
    HSTX_CMD_NOP
};
// An active line is the same blanking preamble, then a TMDS run whose pixel
// data the DMA chains in from the scanline buffer.
static uint32_t active_pre[] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT, CTRL_ACTIVE,
    HSTX_CMD_RAW_REPEAT | H_SYNC,  CTRL_HSYNC,
    HSTX_CMD_RAW_REPEAT | H_BACK,  CTRL_ACTIVE,
    HSTX_CMD_TMDS       | H_ACTIVE
};

// Two scanline buffers of RGB565, pixel-doubled from the console's 320-wide
// canvas to 640. Double-buffered so the DMA reads one while core 1 fills the
// other; a single buffer tears every line.
static uint16_t px_buf[2][H_ACTIVE];
static volatile int fill_idx;

static uint dma_cmd, dma_px;

const char *video_backend_name(void) { return "hstx-dvi"; }

// DVI carries audio the same way the PIO backend does, in data islands. That
// path is not implemented here yet, so report false for now and let the build
// bring up analog PWM: silence would be worse than a jack.
int video_backend_has_inband_audio(void) { return 0; }

void video_backend_set_clock(void) {
    // 252 MHz: ten TMDS bits per pixel at 25.2 MHz, so 640x480 at 60.02 Hz,
    // inside spec tolerance. Also a multiple of 12, so PIO-USB host ports
    // work on this build (unlike the 315 MHz composite one).
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(252000, true);
}

void video_backend_init(void) {
    // Expander: turn each 16-bit RGB565 pixel into three TMDS lanes. NBITS is
    // "bits minus one", and ROT is where the channel's bits start in the
    // word: blue at 0, green at 5, red at 11 for RGB565.
    hstx_ctrl_hw->expand_tmds =
        (4u << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB) |   // blue, 5 bits
        (0u << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB)  |
        (5u << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB) |   // green, 6 bits
        (27u << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB) |    // rotate to bit 5
        (4u << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB) |   // red, 5 bits
        (21u << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB);     // rotate to bit 11

    // Two pixels per 32-bit word, so shift 16 bits between them.
    hstx_ctrl_hw->expand_shift =
        (2u << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB) |
        (16u << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB) |
        (1u << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB) |
        (0u << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB);

    // Serialiser: 5 shifts of 2 bits each per 10-bit TMDS symbol, and a
    // CLKDIV of 5 so the output clock lane runs at the pixel rate.
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        (5u << HSTX_CTRL_CSR_CLKDIV_LSB) |
        (5u << HSTX_CTRL_CSR_N_SHIFTS_LSB) |
        (2u << HSTX_CTRL_CSR_SHIFT_LSB) |
        HSTX_CTRL_CSR_EN_BITS;

    // Lane mapping for an Adafruit-style HSTX DVI breakout: GPIO12/13 are the
    // clock pair, then blue, green, red on 14/15, 16/17, 18/19. A board that
    // wires these differently needs this table changed, and the symptom of
    // getting it wrong is wrong colours or no lock rather than a crash.
    static const struct { int lane; int inv; } pinmap[8] = {
        {0, 0}, {0, 1},        // GPIO12/13: clock +/-
        {1, 0}, {1, 1},        // GPIO14/15: lane 0 (blue)
        {2, 0}, {2, 1},        // GPIO16/17: lane 1 (green)
        {3, 0}, {3, 1},        // GPIO18/19: lane 2 (red)
    };
    for (int i = 0; i < 8; i++) {
        int l = pinmap[i].lane;
        uint32_t v;
        if (l == 0) {
            // Clock lane: a fixed 1010... pattern rather than shifted data.
            v = (0u << HSTX_CTRL_BIT0_SEL_P_LSB) |
                (0u << HSTX_CTRL_BIT0_SEL_N_LSB) |
                HSTX_CTRL_BIT0_CLK_BITS;
        } else {
            v = ((uint32_t)((l - 1) * 10) << HSTX_CTRL_BIT0_SEL_P_LSB) |
                ((uint32_t)((l - 1) * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB);
        }
        if (pinmap[i].inv) v |= HSTX_CTRL_BIT0_INV_BITS;
        hstx_ctrl_hw->bit[i] = v;
        gpio_set_function(12 + i, 0);      // GPIO_FUNC_HSTX
    }

    dma_cmd = dma_claim_unused_channel(true);
    dma_px  = dma_claim_unused_channel(true);
    fill_idx = 0;
}

void video_backend_start_on_core1(void) {
    // The first scanline call starts the stream; nothing to prime here.
}

// Push one command list into the HSTX FIFO and wait for it to drain. Simple
// and synchronous: the sophisticated version chains DMA blocklists so the CPU
// never waits, which is what a real implementation should do once this is
// proven on hardware.
static void push_words(const uint32_t *w, int n) {
    dma_channel_config c = dma_channel_get_default_config(dma_cmd);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(dma_cmd, &c, &hstx_fifo_hw->fifo, w, n, true);
    dma_channel_wait_for_finish_blocking(dma_cmd);
}

void video_backend_scanline(const uint16_t *line16, int y) {
    // Pixel-double 320 -> 640. The console canvas is 320 wide and DVI is
    // emitting 640, so each source pixel covers two output pixels exactly:
    // an integer scale, never a fractional one.
    uint16_t *dst = px_buf[fill_idx];
    for (int x = 0; x < 320; x++) {
        dst[x * 2] = line16[x];
        dst[x * 2 + 1] = line16[x];
    }

    // Each console row covers two DVI rows, for the same reason: 240 -> 480.
    for (int rep = 0; rep < 2; rep++) {
        push_words(active_pre, sizeof active_pre / 4);
        dma_channel_config c = dma_channel_get_default_config(dma_px);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, DREQ_HSTX);
        dma_channel_configure(dma_px, &c, &hstx_fifo_hw->fifo,
                              dst, H_ACTIVE / 2, true);
        dma_channel_wait_for_finish_blocking(dma_px);
    }
    fill_idx ^= 1;

    // The shared scanout loop only knows about 240 rows, so the vertical
    // blanking interval is emitted here after the last visible one.
    if (y == 239) {
        for (int l = 0; l < V_FRONT; l++) push_words(vblank_line, 7);
        for (int l = 0; l < V_SYNC; l++)  push_words(vsync_line, 7);
        for (int l = 0; l < V_BACK; l++)  push_words(vblank_line, 7);
    }
}
