// Video backend interface.
//
// The console supports three ways of getting pixels to a display, and they
// are mutually exclusive at BUILD time because each one dictates clk_sys and
// the whole system hangs off that:
//
//   PIO-DVI   bit-banged TMDS through PIO state machines   252 MHz
//   HSTX-DVI  the RP2350's hardware TMDS encoder, GPIO12-19  240 MHz
//   composite NTSC colour, resistor-DAC into an RCA jack     315 MHz
//
// clk_sys is set once at boot and cannot be retuned without tearing down
// video and USB, so this is a build-time choice, not a runtime one. It also
// has a knock-on: PIO-USB host wants clk_sys to be a multiple of 12 MHz, so
// the 315 MHz composite build cannot drive USB gamepads through PIO and has
// to use the native port or retro shift-register controller ports instead.
//
// What this interface deliberately does NOT change: the game-facing ABI.
// ABI section 3 already states the system clock is a board-config detail
// dictated by the video backend and that games never see it. So a cart
// compiles once and runs on every backend.
//
// Scanout runs on CORE 1 in every backend. That is the load-bearing fact
// behind the ABI's "2.5 million cycles per frame" guarantee: the figure is
// core 0's budget with video already accounted for, which is why the choice
// of backend does not change what a game can afford to do.
#ifndef S32_VIDEO_BACKEND_H
#define S32_VIDEO_BACKEND_H
#include <stdint.h>

// Backend identity, for the launcher to display and for logs. Getting this
// on screen matters: a UF2 built for the wrong backend produces no picture
// at all, and without a version line there is nothing to diagnose from.
const char *video_backend_name(void);

// Set clk_sys and core voltage for this backend, and nothing else. Called
// FIRST, before any peripheral is touched, because every PIO divider and the
// USB 48 MHz reference are derived from clk_sys. This is the call that makes
// the backends mutually exclusive: PIO-DVI wants 252 MHz, HSTX 240, NTSC
// composite 315, and clk_sys cannot be retuned later without tearing down
// video and USB.
void video_backend_set_clock(void);

// Bring the backend up far enough to accept scanout, but do NOT start it.
// Called on core 0 during video_init, before core 1 is launched.
void video_backend_init(void);

// Called ON CORE 1, once, at the top of the scanout loop: register the
// backend's IRQs against this core and start the pixel clock.
void video_backend_start_on_core1(void);

// Hand one converted scanline to the backend. `line16` is 320 pixels of
// RGB565 that the shared scanout loop has already produced from the 8bpp
// framebuffer through the palette, so a backend never touches the console's
// framebuffer format or the palette. `y` is the display row.
//
// This call may block until the backend has a free buffer: that block is
// what paces scanout to the display, and every backend relies on it.
void video_backend_scanline(const uint16_t *line16, int y);

#endif
