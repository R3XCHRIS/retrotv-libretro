#include "loud.h"

#include <math.h>

#define ATTACK_S 0.3       /* getting louder: turn down quickly */
#define RELEASE_S 3.0      /* getting quieter: turn up slowly, no pumping */
#define FAST_S 0.2         /* both ways, just after a reset */
#define FAST_FOR_S 1.5
#define GATE_DB (-55.0)    /* quieter blocks are silence: don't adapt to them */
#define KNEE 0.7           /* limiter: fraction of full scale where it starts */

static double db_to_pow(double db) { return pow(10.0, db / 10.0); }
static double db_to_lin(double db) { return pow(10.0, db / 20.0); }

void loud_reset(loud_state *st)
{
  st->level = db_to_pow(LOUD_TARGET_DB);   /* assume on target: unity gain */
  st->gain = 1.0;
  st->fast_left = FAST_FOR_S;
}

void loud_process(loud_state *st, int16_t *s, size_t frames, int rate)
{
  if (!frames || rate <= 0) return;
  if (st->gain <= 0) loud_reset(st);                /* zeroed state: initialise */
  size_t n = frames * 2;
  double sum = 0;
  for (size_t i = 0; i < n; i++) {
    double x = s[i] / 32768.0;
    sum += x * x;
  }
  double p = sum / (double)n, dt = (double)frames / rate;
  double g0 = st->gain, g1 = g0;
  if (p > db_to_pow(GATE_DB)) {
    double tau = st->fast_left > 0 ? FAST_S : p > st->level ? ATTACK_S : RELEASE_S;
    st->level += (p - st->level) * (1.0 - exp(-dt / tau));
    g1 = sqrt(db_to_pow(LOUD_TARGET_DB) / st->level);
    if (g1 < db_to_lin(LOUD_MIN_DB)) g1 = db_to_lin(LOUD_MIN_DB);
    if (g1 > db_to_lin(LOUD_MAX_DB)) g1 = db_to_lin(LOUD_MAX_DB);
  }
  if (st->fast_left > 0) st->fast_left -= dt;
  for (size_t i = 0; i < n; i++) {
    double g = g0 + (g1 - g0) * (double)(i / 2 + 1) / (double)frames;
    double x = s[i] / 32768.0 * g, a = fabs(x);
    if (a > KNEE) a = KNEE + (1.0 - KNEE) * tanh((a - KNEE) / (1.0 - KNEE));
    s[i] = (int16_t)lrint((x < 0 ? -a : a) * 32767.0);
  }
  st->gain = g1;
}
