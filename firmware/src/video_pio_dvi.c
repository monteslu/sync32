// PIO-DVI backend: bit-banged TMDS through PIO state machines (libdvi).
//
// This is the backend the console shipped with. It needs no extra hardware
// beyond resistors and an HDMI connector, which is why it stays the default,
// but it consumes PIO state machines that an HSTX build leaves free for USB
// host ports and SDIO.
//
// The code here was MOVED out of video.c unchanged rather than rewritten:
// this path has run for hours on real hardware and the HDMI-audio island
// timing in particular was hard-won (see the comments below), so re-deriving
// it behind a new interface would have been the wrong kind of tidy.
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "hardware/watchdog.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "dvi.h"
#include "tmds_encode.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "hdmi_island.h"
#include "hdmi_audio.h"
#include "video_backend.h"

#define DVI_TIMING dvi_timing_640x480p_60hz

const char *video_backend_name(void) { return "pio-dvi"; }

// HDMI data islands carry the audio, so this build needs no analog path.
int video_backend_has_inband_audio(void) { return 1; }

void video_backend_set_clock(void) {
    // 252 MHz. A multiple of 12, so PIO-USB host ports work on this build.
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);
}

static struct dvi_inst dvi0;

// TMDS scanline buffers, static (DVI_N_TMDS_BUFFERS=0): the 9KB heap must
// stay free for FatFs/exFAT. 3 * 640/DVI_SYMBOLS_PER_WORD words each; two
// live in the otherwise-idle 4KB scratch banks (own arbiters: scanout DMA
// never contends with framebuffer traffic), the third in main RAM.
#define TMDS_WORDS (3 * 640 / 2)
static uint32_t tmds_buf_a[TMDS_WORDS] __attribute__((section(".scratch_y.tmdsbuf")));
static uint32_t tmds_buf_b[TMDS_WORDS] __attribute__((section(".scratch_x.tmdsbuf")));
static uint32_t tmds_buf_c[TMDS_WORDS];


void video_backend_start_on_core1(void) {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
}

// The island sits in the BACK PORCH, after the hsync pulse, and the control
// symbols on either side of it are get_ctrl_sym(vsync, !h_sync_polarity).
// 640x480p60 is negative-sync, so the idle level there is hsync=1, vsync=1.
// The island's ch0 nibble must carry the SAME levels or the sync signal
// inverts for 44 clocks of every scanline and no sink will lock onto it.
#define S32_ISLAND_HSYNC (!DVI_TIMING.h_sync_polarity)
#define S32_ISLAND_VSYNC (!DVI_TIMING.v_sync_polarity)
static bool aud_block_start = true;

void video_backend_scanline(const uint16_t *line16, int y) {
    (void)y;
    uint32_t *tmdsbuf;
        queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
        uint pixwidth = dvi0.timing->h_active_pixels;
        uint words_per_channel = pixwidth / DVI_SYMBOLS_PER_WORD;
        const uint32_t *pix = (const uint32_t *)line16;
        tmds_encode_data_channel_16bpp(pix, tmdsbuf + 0 * words_per_channel, pixwidth / 2, DVI_16BPP_BLUE_MSB,  DVI_16BPP_BLUE_LSB );
        tmds_encode_data_channel_16bpp(pix, tmdsbuf + 1 * words_per_channel, pixwidth / 2, DVI_16BPP_GREEN_MSB, DVI_16BPP_GREEN_LSB);
        tmds_encode_data_channel_16bpp(pix, tmdsbuf + 2 * words_per_channel, pixwidth / 2, DVI_16BPP_RED_MSB,   DVI_16BPP_RED_LSB  );
        queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
        // The island sits in the BACK PORCH, after the hsync pulse, and the
        // control symbols on either side of it are get_ctrl_sym(vsync,
        // !h_sync_polarity). 640x480p60 is negative-sync, so the idle level
        // there is hsync=1, vsync=1. The island's ch0 nibble must carry the
        // SAME levels or the sync signal inverts for 44 clocks of every
        // scanline and no sink will lock onto the islands.
        #define S32_ISLAND_HSYNC (!DVI_TIMING.h_sync_polarity)
        #define S32_ISLAND_VSYNC (!DVI_TIMING.v_sync_polarity)
        // HDMI audio: build one data island per scanline while the game is
        // pushing samples. 48kHz over 31500 lines/s needs 1.52 frames per
        // line, so a 4-frame packet every ~3 lines keeps the sink fed.
        if (hdmi_island_is_armed()) {
            static uint16_t ctl_ctr;
            hdmi_packet_t pkt;
            // Control-packet cadence, matched to what the working libdvi
            // HDMI-audio forks actually do rather than to my own reading of
            // the spec text: they send the AVI and Audio InfoFrames once per
            // frame each (~30/s) and ACR once per frame (~60/s), all during
            // vertical blanking. An earlier version of this code chased a
            // "300 ACR/s minimum" and burned 1 island in 32 on control
            // packets; the references show that is unnecessary.
            // 1 island in 96, with ACR taking two of every four slots, lands
            // on the reference rates almost exactly: AVI 30/s, Audio
            // InfoFrame 30/s, ACR 60/s at ~11.5k islands/s.
            if ((ctl_ctr % 96) == 0) {
                unsigned slot = (ctl_ctr / 96) & 3;
                if (slot == 0)      hdmi_pkt_avi_infoframe(&pkt);
                else if (slot == 1) hdmi_pkt_audio_infoframe(&pkt);
                else                hdmi_pkt_acr(&pkt, 6144, 25200);
                ctl_ctr++;
                hdmi_island_build(&pkt, S32_ISLAND_HSYNC, S32_ISLAND_VSYNC);
            } else {
                int16_t pcm[8];
                extern int s32_audio_take(int16_t *out, int max_frames);
                int n = s32_audio_take(pcm, 4);
                if (n > 0) {
                    ctl_ctr++;
                    hdmi_pkt_audio(&pkt, pcm, n, aud_block_start, 0);
                    aud_block_start = false;
                    hdmi_island_build(&pkt, S32_ISLAND_HSYNC, S32_ISLAND_VSYNC);
                }
            }
        }
}

void video_backend_init(void) {
    pio_set_gpio_base(DVI_DEFAULT_SERIAL_CONFIG.pio, 16);
    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
    hdmi_island_init();      // prime island buffers BEFORE the DVI IRQs run
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
    // Hand libdvi its three scanline buffers. Without these the free-queue is
    // empty and the first queue_remove_blocking_u32 in video_backend_scanline
    // blocks forever: a black screen with a live core1, which looks exactly
    // like a dead board.
    void *tb;
    tb = tmds_buf_a; queue_add_blocking_u32(&dvi0.q_tmds_free, (uint32_t *)&tb);
    tb = tmds_buf_b; queue_add_blocking_u32(&dvi0.q_tmds_free, (uint32_t *)&tb);
    tb = tmds_buf_c; queue_add_blocking_u32(&dvi0.q_tmds_free, (uint32_t *)&tb);
}
