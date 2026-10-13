/* loud.h - even out TV loudness: an automatic gain control with a soft limiter.
 * Channels differ a lot (measured: one cartoon channel averages -13 dBFS with
 * peaks at full scale, a 4:3 drama channel -39 dBFS), so a fixed boost either
 * distorts the loud ones or leaves the quiet ones inaudible next to games. */
#ifndef RETROTV_LOUD_H
#define RETROTV_LOUD_H

#include <stddef.h>
#include <stdint.h>

#define LOUD_TARGET_DB (-18.0)   /* average level aimed for (dBFS, RMS) */
#define LOUD_MIN_DB (-6.0)       /* gain limits */
#define LOUD_MAX_DB 18.0

typedef struct {
  double level;        /* smoothed signal power (full scale = 1) */
  double gain;         /* current linear gain */
  double fast_left;    /* seconds of fast tracking left after a reset */
} loud_state;

/* Start over (new channel): gain back to unity, track quickly for a moment. */
void loud_reset(loud_state *st);

/* In place on interleaved stereo S16: adapt the gain to this block's level,
 * apply it (ramped across the block, no zipper noise), soft-limit peaks. */
void loud_process(loud_state *st, int16_t *s, size_t frames, int rate);

#endif
