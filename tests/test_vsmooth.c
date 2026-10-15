/* Host unit tests for video timestamp smoothing. Build: make test */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/vsmooth.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

/* 60 Hz refreshes each frame is held for, when shown at the first refresh at or after its pts */
static int holds(const double *pts, int n, int *out)
{
  int prev = -1;
  for (int i = 0; i < n; i++) {
    int vs = (int)ceil(pts[i] * 60.0 - 1e-9);
    if (prev >= 0) out[i - 1] = vs - prev;
    prev = vs;
  }
  return n - 1;
}

int main(void)
{
  const double fps = 24000.0 / 1001.0, dur = 1.0 / fps;
  enum { N = 2400 };
  static double raw[N], sm[N], ideal[N];
  static int hr[N], hs[N], hi[N];
  vsmooth v;
  vsmooth_reset(&v);
  srand(7);
  for (int i = 0; i < N; i++) {                      /* +-0.8 ms jitter, like the TNG source */
    ideal[i] = 100.0 + i * dur;
    raw[i] = ideal[i] + ((rand() % 1601) - 800) / 1e6;
    sm[i] = vsmooth_pts(&v, raw[i], fps);
  }
  int n = holds(raw, N, hr);
  holds(sm, N, hs);
  holds(ideal, N, hi);
  int bad_raw = 0, bad_sm = 0, bad_ideal = 0;  /* 23.976 fps doesn't fit 60 Hz evenly: some breaks are inherent */
  for (int i = 1; i < n; i++) {                      /* a cadence break: two equal holds in a row */
    bad_raw += hr[i] == hr[i - 1];
    bad_sm += hs[i] == hs[i - 1];
    bad_ideal += hi[i] == hi[i - 1];
  }
  printf("cadence breaks over %d frames: raw %d, smoothed %d, perfect timing %d\n", n, bad_raw, bad_sm, bad_ideal);
  CHECK(bad_raw > 4 * bad_ideal);
  CHECK(bad_sm <= bad_ideal + 1);                    /* as good as perfect timing */
  double maxdev = 0;
  for (int i = 0; i < N; i++) maxdev = fmax(maxdev, fabs(sm[i] - raw[i]));
  CHECK(maxdev < 0.002);                             /* never far from the real time (A/V sync) */

  /* a steady stream at a slightly different rate is followed, not drifted from */
  vsmooth_reset(&v);
  double last = 0;
  for (int i = 0; i < 20000; i++) last = vsmooth_pts(&v, i * (1.0 / 23.95), fps) - i * (1.0 / 23.95);
  CHECK(fabs(last) < 0.003);

  /* jumps resync at once */
  vsmooth_reset(&v);
  CHECK(vsmooth_pts(&v, 10.0, fps) == 10.0);
  CHECK(fabs(vsmooth_pts(&v, 10.0 + dur + 0.0005, fps) - (10.0 + dur + 0.0005 / 32)) < 1e-9);
  CHECK(vsmooth_pts(&v, 500.0, fps) == 500.0);           /* splice / ad break */
  CHECK(vsmooth_pts(&v, 500.0 + 3 * dur, fps) == 500.0 + 3 * dur);   /* lost frames */
  CHECK(vsmooth_pts(&v, 499.0, fps) == 499.0);           /* backwards */
  CHECK(isnan(vsmooth_pts(&v, NAN, fps)));               /* no timestamp: passes, restarts */
  CHECK(vsmooth_pts(&v, 499.1, fps) == 499.1);
  CHECK(vsmooth_pts(&v, 600.0, 0) == 600.0);             /* fps unknown */
  CHECK(vsmooth_pts(&v, 600.0 + dur, 0) == 600.0 + dur);

  /* frame rates */
  CHECK(vsmooth_plausible(90000, 1) == 0);                /* the TNG stream's "tbr" */
  CHECK(fabs(vsmooth_plausible(10000, 417) - 23.98) < 0.01);
  CHECK(vsmooth_plausible(25, 1) == 25 && vsmooth_plausible(0, 1) == 0 && vsmooth_plausible(1, 0) == 0);
  double gaps[VSMOOTH_GAPS];
  for (int i = 0; i < VSMOOTH_GAPS; i++) gaps[i] = raw[i + 1] - raw[i];   /* jittery 23.976 */
  CHECK(vsmooth_rate(gaps, VSMOOTH_GAPS) == 24000.0 / 1001);
  for (int i = 0; i < VSMOOTH_GAPS; i++) gaps[i] = 0.04 + (i % 3 - 1) * 0.0006;
  CHECK(vsmooth_rate(gaps, VSMOOTH_GAPS) == 25);
  gaps[3] = 12.0; gaps[7] = 0; gaps[9] = -1;              /* a splice, a repeat, a step back */
  CHECK(vsmooth_rate(gaps, VSMOOTH_GAPS) == 25);
  CHECK(vsmooth_rate(gaps, 6) == 0);                      /* too few */
  for (int i = 0; i < VSMOOTH_GAPS; i++) gaps[i] = 1.0 / 15;
  CHECK(fabs(vsmooth_rate(gaps, VSMOOTH_GAPS) - 15) < 1e-9);   /* unusual but plausible: kept */

  if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
  printf("test_vsmooth: all passed\n");
  return 0;
}
