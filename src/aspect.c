#include "aspect.h"

#include <stdlib.h>

#define BLACK 32               /* luma at or below this counts as bar (TV black is 16) */
#define MAX_BAR_FRAC 0.40      /* bigger "bars" mean a dark frame, not a letterbox */
#define TRACK_AGREE 4          /* measurements that must agree before bars change */
#define TRACK_SLACK 4          /* pixels of jitter tolerated between measurements */

static int row_black(const uint8_t *luma, int stride, int w, int y)
{
  const uint8_t *r = luma + (size_t)y * stride;
  for (int x = 0; x < w; x += 4)
    if (r[x] > BLACK) return 0;
  return 1;
}

static int col_black(const uint8_t *luma, int stride, int x, int y0, int y1)
{
  for (int y = y0; y < y1; y += 4)
    if (luma[(size_t)y * stride + x] > BLACK) return 0;
  return 1;
}

int aspect_bars(const uint8_t *luma, int stride, int w, int h, int *top, int *left)
{
  int maxv = (int)(h * MAX_BAR_FRAC), maxh = (int)(w * MAX_BAR_FRAC);
  int t = 0, b = 0, l = 0, r = 0;
  while (t <= maxv && row_black(luma, stride, w, t)) t++;
  while (b <= maxv && row_black(luma, stride, w, h - 1 - b)) b++;
  if (t > maxv || b > maxv) return 0;
  /* columns only inside the vertical bars, so a letterbox doesn't look like a pillarbox */
  while (l <= maxh && col_black(luma, stride, l, t, h - b)) l++;
  while (r <= maxh && col_black(luma, stride, w - 1 - r, t, h - b)) r++;
  if (l > maxh || r > maxh) return 0;
  *top = t < b ? t : b;
  *left = l < r ? l : r;
  return 1;
}

void aspect_track_reset(aspect_tracker *t)
{
  t->top = t->left = t->cand_top = t->cand_left = 0;
  t->count = 0;
}

void aspect_track(aspect_tracker *t, int top, int left)
{
  if (abs(top - t->cand_top) <= TRACK_SLACK && abs(left - t->cand_left) <= TRACK_SLACK) {
    if (++t->count >= TRACK_AGREE) {
      /* the smallest agreeing bar wins: never crop into the picture */
      t->top = top < t->cand_top ? top : t->cand_top;
      t->left = left < t->cand_left ? left : t->cand_left;
    }
  } else {
    t->cand_top = top;
    t->cand_left = left;
    t->count = 1;
  }
}

void aspect_zoom_rect(int w, int h, double sar, int top, int left, int *x, int *y, int *cw, int *ch)
{
  if (sar <= 0) sar = 1.0;
  int aw = w - 2 * left, ah = h - 2 * top;          /* active picture */
  if (aw < 2 || ah < 2) { aw = w; ah = h; left = top = 0; }
  double dar = aw * sar / ah;
  int ow = aw, oh = ah;
  if (dar > 4.0 / 3.0) ow = (int)(ah * 4.0 / 3.0 / sar);   /* wide: crop the sides */
  else oh = (int)(aw * sar * 3.0 / 4.0);                    /* tall: crop top and bottom */
  ow &= ~1; oh &= ~1;
  if (ow < 2) ow = 2;
  if (oh < 2) oh = 2;
  *x = (left + (aw - ow) / 2) & ~1;
  *y = (top + (ah - oh) / 2) & ~1;
  *cw = ow;
  *ch = oh;
}
