#include "osd.h"

#include <string.h>

#include "font8x8_basic.h"

static inline uint32_t blend(uint32_t dst, uint32_t src, int a)
{
  if (a >= 255) return src;
  if (a <= 0) return dst;
  uint32_t rb = ((src & 0xff00ff) * (uint32_t)a + (dst & 0xff00ff) * (uint32_t)(255 - a)) / 255;
  uint32_t g  = ((src & 0x00ff00) * (uint32_t)a + (dst & 0x00ff00) * (uint32_t)(255 - a)) / 255;
  return (rb & 0xff00ff) | (g & 0x00ff00);
}

void osd_fill(osd_surface *s, int x, int y, int w, int h, uint32_t rgb, int alpha)
{
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = x + w > s->w ? s->w : x + w, y1 = y + h > s->h ? s->h : y + h;
  for (int j = y0; j < y1; j++) {
    uint32_t *row = s->px + (size_t)j * (size_t)s->pitch;
    for (int i = x0; i < x1; i++) row[i] = blend(row[i], rgb, alpha);
  }
}

int osd_text_width(const char *text, int scale)
{
  return (int)strlen(text) * 8 * scale;
}

int osd_text(osd_surface *s, int x, int y, const char *text, int scale, uint32_t rgb)
{
  int cx = x;
  for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
    unsigned c = *p < 128 ? *p : '?';
    const char *glyph = font8x8_basic[c];
    for (int row = 0; row < 8; row++) {
      unsigned bits = (unsigned char)glyph[row];
      for (int col = 0; col < 8; col++) {
        if (!(bits & (1u << col))) continue;   /* bit 0 is the leftmost pixel */
        int px = cx + col * scale, py = y + row * scale;
        for (int dy = 0; dy < scale; dy++) {
          int yy = py + dy;
          if (yy < 0 || yy >= s->h) continue;
          uint32_t *line = s->px + (size_t)yy * (size_t)s->pitch;
          for (int dx = 0; dx < scale; dx++) {
            int xx = px + dx;
            if (xx >= 0 && xx < s->w) line[xx] = rgb;
          }
        }
      }
    }
    cx += 8 * scale;
  }
  return cx - x;
}

int osd_text_shadow(osd_surface *s, int x, int y, const char *text, int scale, uint32_t rgb)
{
  osd_text(s, x + scale, y + scale, text, scale, OSD_RGB(0, 0, 0));
  return osd_text(s, x, y, text, scale, rgb);
}

void osd_static(osd_surface *s, uint32_t *seed)
{
  uint32_t x = *seed ? *seed : 0x9e3779b9u;
  for (int j = 0; j < s->h; j++) {
    uint32_t *row = s->px + (size_t)j * (size_t)s->pitch;
    /* faint horizontal banding like a detuned set */
    int band = ((j + (int)(x & 63)) / 3) % 7 == 0 ? 30 : 0;
    for (int i = 0; i < s->w; i += 2) {
      x ^= x << 13; x ^= x >> 17; x ^= x << 5;
      int v = (int)(x & 0xff);
      v = v * v / 255 + band;
      if (v > 255) v = 255;
      uint32_t c = OSD_RGB(v, v, v);
      row[i] = c;
      if (i + 1 < s->w) row[i + 1] = c;
    }
  }
  *seed = x;
}
