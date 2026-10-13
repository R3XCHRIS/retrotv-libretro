/* Host unit tests for the loudness boost. Build: make test */
#include <stdio.h>
#include <stdlib.h>

#include "../src/loud.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

int main(void)
{
  int16_t s[6] = { 0, 1000, -1000, 4000, 32767, -32768 };
  loud_apply(s, 6, 12.0);
  CHECK(s[0] == 0);
  CHECK(s[1] > 3900 && s[1] < 4000);           /* quiet audio: +12 dB, about x3.98 */
  CHECK(s[2] == -s[1]);                         /* symmetric */
  CHECK(s[3] > 15000 && s[3] < 16384);         /* below the knee: still linear */
  CHECK(s[4] > 30000 && s[4] <= 32767);        /* full scale: limited, no wrap */
  CHECK(s[5] < -30000 && s[5] >= -32767);
  int16_t ramp[2] = { 8000, 9000 };            /* limiting keeps order (no fold-over) */
  loud_apply(ramp, 2, 12.0);
  CHECK(ramp[1] > ramp[0]);
  if (fails) return 1;
  printf("test_loud: all passed\n");
  return 0;
}
