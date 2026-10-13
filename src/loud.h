/* loud.h - make TV audio as loud as games: fixed gain with a soft limiter.
 * Live TV is mastered around -23 LUFS, games far hotter, and the 5.1 -> stereo
 * downmix lowers the level further, so RetroTV sounded very quiet next to games. */
#ifndef RETROTV_LOUD_H
#define RETROTV_LOUD_H

#include <stddef.h>
#include <stdint.h>

#define LOUD_GAIN_DB 12.0

/* In place: multiply by the gain; peaks above the knee bend smoothly towards
 * full scale instead of clipping. */
void loud_apply(int16_t *s, size_t n, double gain_db);

#endif
