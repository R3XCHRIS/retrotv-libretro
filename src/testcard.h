/* testcard.h - a built-in test card: colour bars and a 1 kHz tone that walks
 * left, right, both (so new users can check picture, sound and stereo). */
#ifndef RETROTV_TESTCARD_H
#define RETROTV_TESTCARD_H

#include <stddef.h>
#include <stdint.h>

#include "osd.h"

#define TESTCARD_TONE_HZ 1000.0
#define TESTCARD_TONE_DB (-18.0)
#define TESTCARD_CYCLE 4.0          /* seconds: left 1 s, right 1 s, both 2 s */

/* Which speakers the tone is on at time t: 1 left, 2 right, 3 both. */
int testcard_side(double t);

/* Fill frames of stereo S16 for time t (seconds since the card started); phase carries over. */
void testcard_audio(int16_t *out, size_t frames, int rate, double t, double *phase);

/* Draw the card; side as returned by testcard_side; clock text drawn in the centre box. */
void testcard_draw(osd_surface *s, int side, const char *clock);

#endif
