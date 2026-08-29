// NTSC composite video backend: colour, 240p, one RCA jack.
//
// STATUS: written 2026-08-29, NEVER RUN ON HARDWARE. The timing below is
// derived and checked against the NTSC-M spec numbers, and every division
// comes out an exact integer (which is the sign the clock plan is right),
// but no scope or TV has confirmed it. Treat colour, and specifically burst
// phase, as unproven until someone plugs it into a CRT.
//
// Why 315 MHz. 315/88 is EXACTLY 3.579545 MHz, the NTSC colour subcarrier.
// Sampling at clk_sys/22 = 14.31818 MHz then gives exactly:
//   4 samples per subcarrier cycle  -> chroma phase is a 4-entry rotation
//   910 samples per 63.5555us line  -> no accumulating line-timing error
//   36-sample colour burst          -> exactly the 9 cycles the spec wants
// Three exact integers falling out of one clock choice is the whole reason
// this is affordable. It is also why composite cannot coexist with HDMI:
// that wants ~252 MHz, and clk_sys is set once at boot.
//
// The signal is built in C into a line buffer and shifted out by PIO through
// a resistor-ladder DAC. Sync, burst and active video are all just DAC
// values, so line structure stays readable here rather than in PIO assembly.
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "video_backend.h"
#include "composite.pio.h"

// ---- pins ----------------------------------------------------------------
// 6-bit R2R ladder on GP2..GP7 into 75 ohm. Six bits gives 64 levels over the
// 1 Vpp NTSC range (~16 mV/step), which is finer than a CRT resolves and
// leaves the DVI pins (32-38), SD (30,31,40,43) and PWM audio (20,21) alone.
#define DAC_PIN_BASE 2
#define DAC_BITS     6

// ---- timing --------------------------------------------------------------
#define SAMPLES_PER_LINE 910
#define LINES_PER_FRAME  262          // 240p: progressive, no interlace

// Segment lengths in samples, from the NTSC-M line at 14.31818 MHz. These sum
// to 909; the spare sample goes on the front porch, which is blanking and the
// one place a sample cannot matter.
#define FP_LEN     22                 // front porch (21 + the rounding spare)
#define SYNC_LEN   67                 // horizontal sync pulse, 4.7us
#define BREEZE_LEN  9                 // breezeway, 0.6us
#define BURST_LEN  36                 // colour burst, exactly 9 subcarrier cycles
#define BP_LEN     23                 // back porch, 1.6us
#define ACTIVE_LEN 753                // active video, 52.6us

// ---- levels --------------------------------------------------------------
// NTSC IRE mapped onto the 6-bit DAC. Sync tip is 0 V and white is 1 V into
// 75 ohm, with blanking at 0 IRE and black at 7.5 IRE (the "setup" pedestal
// that NTSC-M uses and Japan does not).
#define DAC_MAX    ((1 << DAC_BITS) - 1)
#define IRE_TO_DAC(ire) ((uint8_t)(((ire) + 40) * DAC_MAX / 140))
#define LVL_SYNC   IRE_TO_DAC(-40)
#define LVL_BLANK  IRE_TO_DAC(0)
#define LVL_BLACK  IRE_TO_DAC(7.5f)
#define LVL_WHITE  IRE_TO_DAC(100)

// Colour burst: 9 cycles of subcarrier at 180 degrees, +/-20 IRE about
// blanking. With 4 samples per cycle the phase pattern is literally a
// 4-entry table, which is the payoff of the 315 MHz choice.
static const int8_t burst_phase[4] = {0, -20, 0, +20};

// Two line buffers, DMA reads one while the CPU fills the other. 910 bytes
// each is cheap; the alternative (one buffer) tears every line.
static uint8_t line_buf[2][SAMPLES_PER_LINE];
static volatile int fill_idx;

static PIO cpio = pio0;
static uint csm, cdma;

const char *video_backend_name(void) { return "composite"; }

// Composite carries VIDEO ONLY. The yellow RCA has no audio in it, so these
// builds bring up analog PWM audio instead (audio_pwm.h).
int video_backend_has_inband_audio(void) { return 0; }

void video_backend_set_clock(void) {
    // 315 MHz is an overclock. The lab sweep showed 315 < 330 works at 1.15 V
    // with margin, and xrip's project runs it at stock voltage, so 1.15 V is
    // conservative rather than brave.
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(10);
    set_sys_clock_khz(315000, true);
    // NOTE: 315 is NOT a multiple of 12, so PIO-USB host ports do not work on
    // this build. Controllers must come from the native USB port or from
    // retro shift-register pads. That is a real constraint of colour NTSC,
    // not an oversight.
}

// Fill the blanking part of a line: everything except active video. This is
// identical on every visible line, so it is written once per buffer swap.
static void write_blanking(uint8_t *b) {
    int i = 0;
    for (int k = 0; k < FP_LEN; k++)     b[i++] = LVL_BLANK;
    for (int k = 0; k < SYNC_LEN; k++)   b[i++] = LVL_SYNC;
    for (int k = 0; k < BREEZE_LEN; k++) b[i++] = LVL_BLANK;
    for (int k = 0; k < BURST_LEN; k++)  b[i++] = IRE_TO_DAC(burst_phase[k & 3]);
    for (int k = 0; k < BP_LEN; k++)     b[i++] = LVL_BLANK;
}
#define ACTIVE_START (FP_LEN + SYNC_LEN + BREEZE_LEN + BURST_LEN + BP_LEN)

// Vertical sync: NTSC wants equalising pulses and serrations. A CRT will lock
// to a simplified version, and 240p is already a non-broadcast signal that
// every TV tolerates, so this uses the simple form: whole lines of sync.
static void write_vsync_line(uint8_t *b) {
    for (int i = 0; i < SAMPLES_PER_LINE; i++) b[i] = LVL_SYNC;
    // serration: brief return to blanking mid-line so the CRT keeps its
    // horizontal oscillator running through vertical sync
    for (int i = SAMPLES_PER_LINE / 2; i < SAMPLES_PER_LINE / 2 + SYNC_LEN; i++)
        b[i] = LVL_BLANK;
}

// RGB565 -> composite samples for one pixel run.
//
// Luma is the standard NTSC weighting. Chroma is the colour-difference pair
// modulated onto the subcarrier; with 4 samples per cycle the modulation is
// +I, +Q, -I, -Q in rotation, so no trig and no table beyond the sign.
static inline void encode_pixel(uint8_t *dst, int n, int r, int g, int b,
                                int phase) {
    // 0..255 per channel
    int y = (299 * r + 587 * g + 114 * b) / 1000;          // luma 0..255
    int i = (596 * r - 274 * g - 322 * b) / 1000;          // I
    int q = (211 * r - 523 * g + 312 * b) / 1000;          // Q

    // Luma to IRE: black at 7.5, white at 100.
    int y_ire = 7 + (y * 93) / 255;
    for (int k = 0; k < n; k++) {
        // Chroma amplitude scaled to keep the sum inside the 1 Vpp envelope.
        int c;
        switch ((phase + k) & 3) {
        case 0:  c =  i / 8; break;
        case 1:  c =  q / 8; break;
        case 2:  c = -i / 8; break;
        default: c = -q / 8; break;
        }
        int ire = y_ire + c;
        if (ire < -40) ire = -40; else if (ire > 100) ire = 100;
        dst[k] = IRE_TO_DAC(ire);
    }
}

void video_backend_init(void) {
    uint off = pio_add_program(cpio, &composite_dac_program);
    csm = pio_claim_unused_sm(cpio, true);
    composite_dac_program_init(cpio, csm, off, DAC_PIN_BASE, DAC_BITS);

    for (int i = 0; i < 2; i++) {
        memset(line_buf[i], LVL_BLANK, SAMPLES_PER_LINE);
        write_blanking(line_buf[i]);
    }

    cdma = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(cdma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(cpio, csm, true));
    dma_channel_configure(cdma, &c, &cpio->txf[csm], line_buf[0],
                          SAMPLES_PER_LINE, false);
    fill_idx = 0;
}

void video_backend_start_on_core1(void) {
    // Nothing to start beyond the DMA: the first scanline call kicks it.
}

// One visible line. `y` is 0..239; the vertical blanking interval is emitted
// around the visible field by counting lines here rather than asking the
// shared scanout loop to know anything about NTSC.
void video_backend_scanline(const uint16_t *line16, int y) {
    uint8_t *b = line_buf[fill_idx];
    uint8_t *act = b + ACTIVE_START;

    // 753 active samples across 320 pixels is 2.353 samples/pixel, so step a
    // fixed-point accumulator rather than rounding per pixel: rounding drifts
    // and the picture shears toward the right edge.
    uint32_t acc = 0;
    const uint32_t step = (320u << 16) / ACTIVE_LEN;
    for (int s = 0; s < ACTIVE_LEN; ) {
        int px = (int)(acc >> 16);
        if (px > 319) px = 319;
        uint16_t v = line16[px];
        int r = ((v >> 11) & 0x1F) << 3;
        int g = ((v >> 5) & 0x3F) << 2;
        int bl = (v & 0x1F) << 3;
        // Emit up to 4 samples at a time so the chroma phase table stays
        // aligned to the subcarrier.
        int n = 4;
        if (s + n > ACTIVE_LEN) n = ACTIVE_LEN - s;
        encode_pixel(act + s, n, r, g, bl, s & 3);
        s += n;
        acc += step * (uint32_t)n;
    }

    // Wait for the previous line to finish, then send this one.
    dma_channel_wait_for_finish_blocking(cdma);
    dma_channel_set_read_addr(cdma, b, true);
    fill_idx ^= 1;

    // After the last visible line, emit the vertical blanking interval. The
    // shared scanout loop only knows about 240 rows, so VBI is produced here.
    if (y == 239) {
        static uint8_t vbi[SAMPLES_PER_LINE];
        for (int l = 0; l < LINES_PER_FRAME - 240; l++) {
            if (l < 3) write_vsync_line(vbi);
            else { memset(vbi, LVL_BLANK, SAMPLES_PER_LINE); write_blanking(vbi); }
            dma_channel_wait_for_finish_blocking(cdma);
            dma_channel_set_read_addr(cdma, vbi, true);
        }
    }
}
