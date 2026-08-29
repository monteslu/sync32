// Analog audio out via filtered PWM, for builds with no HDMI to carry it.
//
// The HDMI backends put audio in data islands inside the video signal, so
// there is nothing else to wire. A composite build has no such channel: the
// yellow RCA carries video ONLY, and audio has to leave the board on its own
// pins. This is that path.
//
// How it works: a PWM slice runs far above the audio band and the DUTY CYCLE
// encodes the sample. An external resistor+capacitor low-pass averages that
// switching into a smooth voltage, which is the audio waveform. The filter is
// what makes it audio rather than an ultrasonic square wave, so it is not
// optional (see the wiring note below).
//
// Quality is roughly 8-11 effective bits: fine for the chiptune and sample
// audio these emulated systems produce, not hi-fi. That is the right trade
// for a build whose whole point is costing a dollar in passives.
#ifndef S32_AUDIO_PWM_H
#define S32_AUDIO_PWM_H
#include <stdint.h>

// Wiring, per channel:
//
//   GPIO ---[ 1k ]---+--- audio out (RCA centre / 3.5mm tip)
//                    |
//                  [ 10nF ]
//                    |
//                   GND (RCA shell / sleeve)
//
// 1k and 10nF give a corner near 16 kHz: it passes the audio band and
// attenuates the PWM carrier hard. Add a 1uF series capacitor at the output
// if the sink dislikes the DC offset; most line inputs are already coupled.
//
// Stereo is two of these on two GPIOs. Mono is one: wire the LEFT channel
// only, or set S32_PWM_MONO to sum both sides so nothing is lost. Of the
// systems this console emulates only Game Boy and Game Gear are genuinely
// stereo, so a single jack is a reasonable build.
#define S32_PWM_PIN_L 20
#define S32_PWM_PIN_R 21

// Start the PWM slices and the 48 kHz sample timer. Safe to call when the
// build has no PWM audio: then it is simply never called.
void s32_audio_pwm_init(void);

// True once init has run, so the launcher can report the audio path.
int s32_audio_pwm_active(void);

#endif
