// Filtered-PWM analog audio. See audio_pwm.h for the wiring and the reason
// this exists at all (composite builds have no HDMI data islands to ride in).
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "audio_pwm.h"

extern int s32_audio_take(int16_t *out, int max_frames);

// 11-bit PWM. The carrier is clk_sys / 2048, which is 154 kHz on the 315 MHz
// composite build and 123 kHz on 252 MHz: both far above the audio band, so
// the RC filter removes them without touching the signal. Trading a bit for a
// higher carrier is not worth it here; the noise floor of an 8-bit console's
// own output is well above the quantisation.
#define PWM_BITS 11
#define PWM_WRAP ((1u << PWM_BITS) - 1u)
#define PWM_MID  (1u << (PWM_BITS - 1))

static uint slice_l, slice_r, slice_timer;
static int active;

int s32_audio_pwm_active(void) { return active; }

// Fires at 48 kHz. Keep this SHORT: it runs on whichever core called init,
// alongside game code, and a slow handler here is heard as distortion rather
// than seen as a dropped frame.
static void __not_in_flash_func(sample_isr)(void) {
    pwm_clear_irq(slice_timer);

    int16_t lr[2];
    // One frame at a time. The ring is single-producer/single-consumer and
    // this is the only consumer on a PWM build, exactly as the HDMI island
    // pump is the only consumer on an HDMI build.
    if (s32_audio_take(lr, 1) != 1) {
        // Underrun: hold the midpoint rather than jumping to zero. A jump to
        // rail is a click; silence at mid-scale is silence.
        pwm_set_gpio_level(S32_PWM_PIN_L, PWM_MID);
        pwm_set_gpio_level(S32_PWM_PIN_R, PWM_MID);
        return;
    }

    // s16 sample -> unsigned duty around the midpoint. The >> shift is the
    // 16-bit-to-11-bit reduction; the + PWM_MID recentres it, because a PWM
    // duty cannot be negative and silence has to sit at half rail.
    int l = (lr[0] >> (16 - PWM_BITS)) + (int)PWM_MID;
    int r = (lr[1] >> (16 - PWM_BITS)) + (int)PWM_MID;
    if (l < 0) l = 0; else if (l > (int)PWM_WRAP) l = PWM_WRAP;
    if (r < 0) r = 0; else if (r > (int)PWM_WRAP) r = PWM_WRAP;

    pwm_set_gpio_level(S32_PWM_PIN_L, (uint16_t)l);
    pwm_set_gpio_level(S32_PWM_PIN_R, (uint16_t)r);
}

void s32_audio_pwm_init(void) {
    gpio_set_function(S32_PWM_PIN_L, GPIO_FUNC_PWM);
    gpio_set_function(S32_PWM_PIN_R, GPIO_FUNC_PWM);
    slice_l = pwm_gpio_to_slice_num(S32_PWM_PIN_L);
    slice_r = pwm_gpio_to_slice_num(S32_PWM_PIN_R);

    pwm_config c = pwm_get_default_config();
    pwm_config_set_wrap(&c, PWM_WRAP);
    // No clock divider: the carrier should be as high as the counter allows.
    pwm_config_set_clkdiv(&c, 1.0f);
    pwm_init(slice_l, &c, true);
    if (slice_r != slice_l) pwm_init(slice_r, &c, true);

    pwm_set_gpio_level(S32_PWM_PIN_L, PWM_MID);
    pwm_set_gpio_level(S32_PWM_PIN_R, PWM_MID);

    // The 48 kHz sample tick comes from a SECOND slice used purely as a
    // timer, not from the audio slices themselves: those wrap at the carrier
    // rate (154 kHz), which is not the sample rate. Driving samples off the
    // carrier would resample the stream to the wrong rate, and against the
    // 48 kHz the ABI fixes that is an audible pitch error.
    //
    // Wrap for 48 kHz: clk_sys / (div * wrap) = 48000.
    uint32_t clk = clock_get_hz(clk_sys);
    float div = (float)clk / (48000.0f * 4096.0f);
    if (div < 1.0f) div = 1.0f;
    uint16_t wrap = (uint16_t)((float)clk / (div * 48000.0f)) - 1;

    // Reuse the left slice's IRQ if the timer slice would collide; otherwise
    // pick a slice that no audio pin occupies.
    uint tslice = (slice_l + 1) % NUM_PWM_SLICES;
    while (tslice == slice_l || tslice == slice_r)
        tslice = (tslice + 1) % NUM_PWM_SLICES;

    pwm_config t = pwm_get_default_config();
    pwm_config_set_clkdiv(&t, div);
    pwm_config_set_wrap(&t, wrap);
    // Keep slice_l pointing at the LEFT AUDIO slice: the ISR still writes
    // levels through it. The timer slice gets its own variable, because
    // clobbering slice_l here silently sent every sample to the wrong slice.
    slice_timer = tslice;
    pwm_clear_irq(tslice);
    pwm_set_irq_enabled(tslice, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, sample_isr);
    irq_set_enabled(PWM_IRQ_WRAP, true);
    pwm_init(tslice, &t, true);

    active = 1;
}
