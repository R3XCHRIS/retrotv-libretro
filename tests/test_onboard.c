/* Host unit tests for onboarding hints and the test card. Build: make test */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/onboard.h"
#include "../src/testcard.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static int problem_has(ob_kind k, const char *err, const char *want)
{
  char p[96], h[256];
  onboard_explain(k, err, p, sizeof(p), h, sizeof(h));
  return strstr(p, want) != NULL && *h;
}

int main(void)
{
  /* FFmpeg's error texts as seen on the Pi */
  CHECK(problem_has(OB_FETCH, "fetch failed: Server returned 404 Not Found", "404"));
  CHECK(problem_has(OB_FETCH, "fetch failed: Server returned 403 Forbidden (access denied)", "denied"));
  CHECK(problem_has(OB_FETCH, "fetch failed: Connection refused", "refused"));
  CHECK(problem_has(OB_FETCH, "fetch failed: Connection timed out", "didn't answer"));
  CHECK(problem_has(OB_FETCH, "fetch failed: Protocol not found", "isn't a link"));
  CHECK(problem_has(OB_FETCH, "cannot read /media/nfs/x.m3u", "not found"));
  CHECK(problem_has(OB_FETCH, "something odd", "Couldn't load"));
  CHECK(problem_has(OB_NO_FILE, NULL, "No playlist"));
  CHECK(problem_has(OB_EMPTY_FILE, NULL, "empty"));
  CHECK(problem_has(OB_NOT_M3U, NULL, "isn't a playlist"));
  CHECK(problem_has(OB_NO_CHANNELS, NULL, "no channels"));

  CHECK(onboard_is_m3u("#EXTM3U x-tvg-url=\"a\"\n#EXTINF:-1,A\nhttp://x\n"));
  CHECK(onboard_is_m3u("\xEF\xBB\xBF#EXTM3U\n"));                  /* BOM */
  CHECK(onboard_is_m3u("http://a/1.ts\nhttp://a/2.ts\n"));          /* bare URL list */
  CHECK(!onboard_is_m3u("<!DOCTYPE html><html>login</html>"));
  CHECK(!onboard_is_m3u("") && !onboard_is_m3u(NULL));

  /* tone: left, right, both; about -18 dBFS RMS when on */
  CHECK(testcard_side(0.5) == 1 && testcard_side(1.5) == 2 && testcard_side(3.0) == 3 && testcard_side(4.2) == 1);
  enum { N = 48000 };
  static int16_t buf[N * 2];
  double ph = 0, sl = 0, sr = 0;
  testcard_audio(buf, N, 48000, 2.0, &ph);                           /* "both" second */
  for (int i = 0; i < N; i++) { sl += (double)buf[2 * i] * buf[2 * i]; sr += (double)buf[2 * i + 1] * buf[2 * i + 1]; }
  double db = 10 * log10(sl / N / (32768.0 * 32768.0));
  CHECK(fabs(db - TESTCARD_TONE_DB) < 0.3 && fabs(sl - sr) < sl * 1e-6);
  testcard_audio(buf, 4800, 48000, 0.2, &ph);                        /* "left" */
  int right_silent = 1;
  for (int i = 0; i < 4800; i++) right_silent &= buf[2 * i + 1] == 0;
  CHECK(right_silent);

  uint32_t *px = calloc(640 * 480, 4);                               /* draws without crashing */
  osd_surface s = { px, 640, 480, 640, 0 };
  testcard_draw(&s, 3, "20:40");
  CHECK(px[10 * 640 + 10] == OSD_RGB(191, 191, 191) && px[10 * 640 + 630] == OSD_RGB(0, 0, 191));
  free(px);

  if (fails) return 1;
  printf("test_onboard: all passed\n");
  return 0;
}
