#include "vsmooth.h"

#include <math.h>
#include <stdlib.h>

#define FOLLOW 32.0     /* the grid moves 1/32 of each frame's error toward the real time */

void vsmooth_reset(vsmooth *v) { v->last = NAN; }

double vsmooth_pts(vsmooth *v, double pts, double fps)
{
  if (isnan(pts) || !(fps >= 10 && fps <= 120)) { v->last = NAN; return pts; }
  double dur = 1.0 / fps;
  if (!isnan(v->last)) {
    double err = pts - (v->last + dur);
    if (fabs(err) < dur / 2) {                     /* jitter: stay on the grid */
      v->last += dur + err / FOLLOW;
      return v->last;
    }
  }
  v->last = pts;                                   /* first frame, splice, gap: resync */
  return pts;
}

double vsmooth_plausible(int num, int den)
{
  if (num <= 0 || den <= 0) return 0;
  double f = (double)num / den;
  return f >= 5 && f <= 120 ? f : 0;
}

static int cmp_double(const void *a, const void *b)
{
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

double vsmooth_rate(const double *gaps, int n)
{
  double g[VSMOOTH_GAPS];
  int k = 0;
  for (int i = 0; i < n && k < VSMOOTH_GAPS; i++)
    if (gaps[i] > 1.0 / 121 && gaps[i] < 1.0 / 4) g[k++] = gaps[i];   /* skip jumps and repeats */
  if (k < 8) return 0;
  qsort(g, (size_t)k, sizeof(g[0]), cmp_double);
  double f = 1.0 / g[k / 2];
  static const double STD[] = { 24000.0 / 1001, 24, 25, 30000.0 / 1001, 30, 48, 50, 60000.0 / 1001, 60, 100, 120 };
  for (size_t i = 0; i < sizeof(STD) / sizeof(STD[0]); i++)
    if (fabs(f - STD[i]) < STD[i] * 0.01) return STD[i];
  return f >= 5 && f <= 120 ? f : 0;
}
