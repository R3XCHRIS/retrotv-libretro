/* Host unit tests for the loudness AGC. Build: make test */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/loud.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

enum { RATE = 48000, BLOCK = 800 };   /* one 60 Hz frame of audio */
static double phase;

/* a 440 Hz tone at the given RMS level (dBFS) */
static void tone(int16_t *s, double rms_db)
{
  double amp = pow(10.0, rms_db / 20.0) * sqrt(2.0) * 32767.0;
  for (int i = 0; i < BLOCK; i++, phase += 2 * M_PI * 440.0 / RATE)
    s[2 * i] = s[2 * i + 1] = (int16_t)lrint(amp * sin(phase));
}

/* run for `seconds`; returns output RMS (dBFS) over the last second and the peak */
static double run(loud_state *st, double rms_db, double seconds, int *peak)
{
  int16_t s[BLOCK * 2];
  int blocks = (int)(seconds * RATE / BLOCK), last = RATE / BLOCK;
  double sum = 0; long cnt = 0;
  *peak = 0;
  for (int b = 0; b < blocks; b++) {
    tone(s, rms_db);
    loud_process(st, s, BLOCK, RATE);
    for (int i = 0; i < BLOCK * 2; i++) {
      if (abs(s[i]) > *peak) *peak = abs(s[i]);
      if (b >= blocks - last) { double x = s[i] / 32768.0; sum += x * x; cnt++; }
    }
  }
  return 10 * log10(sum / cnt);
}

int main(void)
{
  loud_state st;
  int peak;

  loud_reset(&st);                                   /* hot channel (-13 dB): turned down to target */
  double hot = run(&st, -13.0, 6, &peak);
  CHECK(fabs(hot - LOUD_TARGET_DB) < 1.5);
  CHECK(peak <= 32767);

  loud_reset(&st);                                   /* quiet channel (-29 dB): up to target */
  double mid = run(&st, -29.0, 15, &peak);
  CHECK(fabs(mid - LOUD_TARGET_DB) < 1.5);

  loud_reset(&st);                                   /* very quiet (-39 dB): capped at +18 dB */
  double low = run(&st, -39.0, 20, &peak);
  CHECK(fabs(low - (-39.0 + LOUD_MAX_DB)) < 1.5);

  int16_t z[BLOCK * 2] = { 0 };                      /* silence: no NaN, gain unchanged */
  double g = st.gain;
  for (int i = 0; i < 300; i++) loud_process(&st, z, BLOCK, RATE);
  CHECK(st.gain == g && z[0] == 0);

  /* a quiet scene then a loud one on the same channel: limiter catches the jump */
  loud_reset(&st);
  run(&st, -35.0, 20, &peak);
  run(&st, -12.0, 0.1, &peak);
  CHECK(peak <= 32767);
  double after = run(&st, -12.0, 2, &peak);         /* and the gain comes down within ~1 s */
  CHECK(fabs(after - LOUD_TARGET_DB) < 1.5);

  if (fails) return 1;
  printf("test_loud: all passed (hot %.1f, quiet %.1f, very quiet %.1f dBFS)\n", hot, mid, low);
  return 0;
}
