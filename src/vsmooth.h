/* vsmooth.h - even out jittery video timestamps.
 * Some sources (e.g. HEVC passed straight through by Dispatcharr's Proxy
 * profile) stamp 23.98 fps frames 41-42 ms apart instead of a steady 41.7.
 * On a 60 Hz screen that breaks the 2-3-2-3 refresh cadence into visible
 * stutter. Frames close to the predicted time are put on the frame grid,
 * which follows the real timestamps slowly; real jumps reset it. */
#ifndef RETROTV_VSMOOTH_H
#define RETROTV_VSMOOTH_H

typedef struct {
  double last;     /* last output pts; NAN = start fresh */
} vsmooth;

void vsmooth_reset(vsmooth *v);

/* Smoothed pts for a frame stamped pts, at fps frames per second. pts NAN,
 * or fps unknown or implausible, passes through (and restarts the grid). */
double vsmooth_pts(vsmooth *v, double pts, double fps);

/* A stream's declared rate (num/den) if it is a plausible frame rate
 * (5-120 fps), else 0. Some streams declare their 90 kHz clock instead. */
double vsmooth_plausible(int num, int den);

/* Frame rate from n frame-to-frame gaps in seconds: the median gap, snapped
 * to a broadcast rate (23.976, 25, 29.97, 50, 59.94, ...) within 1 %.
 * 0 if there aren't enough sensible gaps. */
#define VSMOOTH_GAPS 16
double vsmooth_rate(const double *gaps, int n);

#endif
