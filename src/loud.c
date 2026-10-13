#include "loud.h"

#include <math.h>

#define KNEE 0.5        /* fraction of full scale where limiting starts */

void loud_apply(int16_t *s, size_t n, double gain_db)
{
  double g = pow(10.0, gain_db / 20.0);
  for (size_t i = 0; i < n; i++) {
    double x = s[i] / 32768.0 * g;
    double a = fabs(x);
    if (a > KNEE) a = KNEE + (1.0 - KNEE) * tanh((a - KNEE) / (1.0 - KNEE));
    double y = (x < 0 ? -a : a) * 32767.0;
    s[i] = (int16_t)lrint(y);
  }
}
