/* aspect.h - black bar detection and the 4:3 "zoom" crop (TV aspect button).
 * Pure C with no FFmpeg dependency, so it can be unit tested on the host. */
#ifndef RETROTV_ASPECT_H
#define RETROTV_ASPECT_H

#include <stdint.h>

/* Measure symmetric black bars on an 8-bit luma plane: *top is the smaller of
 * the top/bottom bars, *left the smaller of the left/right bars, in pixels.
 * Returns 0 when the frame is too dark to tell (a fade or night scene). */
int aspect_bars(const uint8_t *luma, int stride, int w, int h, int *top, int *left);

/* Bars only change after several agreeing measurements, so a dark scene or a
 * caption over the bar doesn't make the picture jump. */
typedef struct {
  int top, left;          /* committed bars */
  int cand_top, cand_left;
  int count;
} aspect_tracker;

void aspect_track_reset(aspect_tracker *t);
void aspect_track(aspect_tracker *t, int top, int left);

/* Crop rectangle for ZOOM: the picture inside the bars, centre-cropped to a
 * 4:3 display aspect (sar = sample aspect ratio, 1.0 for square pixels).
 * x, y, cw, ch come out even so 4:2:0 chroma stays aligned. */
void aspect_zoom_rect(int w, int h, double sar, int top, int left, int *x, int *y, int *cw, int *ch);

#endif
