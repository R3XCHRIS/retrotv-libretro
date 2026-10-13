#include "testcard.h"

#include <math.h>
#include <string.h>

int testcard_side(double t)
{
  double c = fmod(t < 0 ? 0 : t, TESTCARD_CYCLE);
  return c < 1.0 ? 1 : c < 2.0 ? 2 : 3;
}

void testcard_audio(int16_t *out, size_t frames, int rate, double t, double *phase)
{
  double amp = pow(10.0, TESTCARD_TONE_DB / 20.0) * sqrt(2.0) * 32767.0;
  double step = 2.0 * M_PI * TESTCARD_TONE_HZ / rate;
  for (size_t i = 0; i < frames; i++) {
    int side = testcard_side(t + (double)i / rate);
    int16_t v = (int16_t)lrint(amp * sin(*phase));
    out[2 * i] = side & 1 ? v : 0;
    out[2 * i + 1] = side & 2 ? v : 0;
    *phase += step;
    if (*phase > 2.0 * M_PI) *phase -= 2.0 * M_PI;
  }
}

void testcard_draw(osd_surface *s, int side, const char *clock)
{
  /* 75% colour bars (EBU order) over the top two thirds */
  static const uint32_t bars[7] = {
    OSD_RGB(191, 191, 191), OSD_RGB(191, 191, 0), OSD_RGB(0, 191, 191), OSD_RGB(0, 191, 0),
    OSD_RGB(191, 0, 191), OSD_RGB(191, 0, 0), OSD_RGB(0, 0, 191),
  };
  int w = s->w, h = s->h, top = h * 2 / 3;
  for (int i = 0; i < 7; i++) osd_fill(s, i * w / 7, 0, (i + 1) * w / 7 - i * w / 7, top, bars[i], 255);
  /* lower third: reverse bars strip, then a black-to-white step wedge */
  static const uint32_t rev[7] = {
    OSD_RGB(0, 0, 191), OSD_RGB(19, 19, 19), OSD_RGB(191, 0, 191), OSD_RGB(19, 19, 19),
    OSD_RGB(0, 191, 191), OSD_RGB(19, 19, 19), OSD_RGB(191, 191, 191),
  };
  int strip = h / 12;
  for (int i = 0; i < 7; i++) osd_fill(s, i * w / 7, top, (i + 1) * w / 7 - i * w / 7, strip, rev[i], 255);
  for (int i = 0; i < 6; i++) {
    int g = 16 + i * 219 / 5;
    osd_fill(s, i * w / 6, top + strip, (i + 1) * w / 6 - i * w / 6, h - top - strip, OSD_RGB(g, g, g), 255);
  }
  /* centre box: name, clock, tone and which speaker */
  int bw = 320, bh = 116, bx = (w - bw) / 2, by = (top - bh) / 2;
  osd_fill(s, bx, by, bw, bh, OSD_RGB(8, 16, 72), 255);
  osd_fill(s, bx, by, bw, 3, OSD_RGB(240, 200, 40), 255);
  const char *t = "RETROTV";
  osd_text_shadow(s, bx + (bw - osd_text_width(t, 4)) / 2, by + 14, t, 4, OSD_RGB(255, 255, 255));
  if (clock && *clock) osd_text_shadow(s, bx + (bw - osd_text_width(clock, 2)) / 2, by + 54, clock, 2, OSD_RGB(255, 214, 64));
  const char *tone = side == 1 ? "1 kHz  LEFT" : side == 2 ? "1 kHz  RIGHT" : "1 kHz  LEFT + RIGHT";
  osd_text(s, bx + (bw - osd_text_width(tone, 2)) / 2, by + 84, tone, 2, OSD_RGB(160, 200, 255));
}
