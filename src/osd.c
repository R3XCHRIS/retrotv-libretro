#include "osd.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "font8x8_basic.h"

static inline uint32_t blend(uint32_t dst, uint32_t src, int a)
{
  if (a >= 255) return src;
  if (a <= 0) return dst;
  /* per channel: dividing packed red+blue by 255 leaked red's remainder into blue */
  uint32_t out = 0;
  for (int sh = 0; sh <= 16; sh += 8) {
    uint32_t sc = (src >> sh) & 0xff, dc = (dst >> sh) & 0xff;
    out |= ((sc * (uint32_t)a + dc * (uint32_t)(255 - a) + 127) / 255) << sh;
  }
  return out;
}

void osd_fill(osd_surface *s, int x, int y, int w, int h, uint32_t rgb, int alpha)
{
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = x + w > s->w ? s->w : x + w, y1 = y + h > s->h ? s->h : y + h;
  y0 >>= s->vshift;                                  /* logical -> buffer rows */
  y1 >>= s->vshift;
  if (y1 <= y0 && h > 0 && y0 < osd_rows(s)) y1 = y0 + 1;   /* thin rules survive halving */
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
        /* rows per glyph row: scale, halved in 240p but never below 1 */
        int rs = scale >> s->vshift ? scale >> s->vshift : 1;
        int px = cx + col * scale, py = (y >> s->vshift) + row * rs;
        for (int dy = 0; dy < rs; dy++) {
          int yy = py + dy;
          if (yy < 0 || yy >= osd_rows(s)) continue;
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
  int dy = s->vshift && scale < 2 ? 2 : scale;       /* a full buffer row down in 240p */
  osd_text(s, x + scale, y + dy, text, scale, OSD_RGB(0, 0, 0));
  return osd_text(s, x, y, text, scale, rgb);
}

void osd_static(osd_surface *s, uint32_t *seed)
{
  uint32_t x = *seed ? *seed : 0x9e3779b9u;
  for (int j = 0; j < osd_rows(s); j++) {
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

void osd_static_vhs(osd_surface *s, uint32_t *seed, unsigned frame)
{
  int rows = osd_rows(s);
  int band = (int)((frame * 3u) % (unsigned)(rows + 60)) - 30;      /* tracking band rolls down */
  for (int y = 0; y < rows; y++) {
    uint32_t *row = s->px + (size_t)y * (size_t)s->pitch;
    int dist = y - band, in_band = dist > -18 && dist < 18;
    *seed ^= *seed << 13; *seed ^= *seed >> 17; *seed ^= *seed << 5;
    int shift = (int)(*seed % 7) - 3;                              /* line jitter */
    int lift = in_band ? 90 - (dist < 0 ? -dist : dist) * 4 : 0;
    for (int x = 0; x < s->w; x++) {
      *seed ^= *seed << 13; *seed ^= *seed >> 17; *seed ^= *seed << 5;
      int v = (int)(*seed & 0xff) * 3 / 4 + lift + ((x + shift) & 8 ? 6 : 0);
      int r = v + (int)((*seed >> 8) & 31) - 16, b = v + (int)((*seed >> 13) & 31) - 16;   /* chroma noise */
      if (v > 255) v = 255;
      r = r < 0 ? 0 : r > 255 ? 255 : r;
      b = b < 0 ? 0 : b > 255 ? 255 : b;
      row[x] = OSD_RGB(r, v, b);
    }
  }
}

static uint32_t xs32(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

/* a row moved off pixels to the right (left if negative); the gap is black, as on tape */
static void shift_row(uint32_t *row, uint32_t *tmp, int w, int off)
{
  if (!off) return;
  for (int x = 0; x < w; x++) {
    int sx = x - off;
    tmp[x] = sx >= 0 && sx < w ? row[sx] : 0;
  }
  memcpy(row, tmp, (size_t)w * sizeof(*row));
}

void osd_vhs_pause(osd_surface *s, uint32_t *seed, unsigned frame)
{
  enum { MAXW = 2048 };
  uint32_t tmp[MAXW];
  int rows = osd_rows(s), w = s->w, k = s->vshift ? 2 : 1;      /* logical pixels per buffer row */
  if (w > MAXW || rows < 8) return;

  /* the still frame shakes: a row up or down for a few frames, now and then */
  uint32_t h = (frame / 3) * 2654435761u;
  int dy = (h >> 28) < 5 ? (int)(h >> 26 & 1) * 2 - 1 : 0;
  if (dy > 0) {
    memmove(s->px + (size_t)s->pitch, s->px, (size_t)(rows - 1) * (size_t)s->pitch * sizeof(*s->px));
    memset(s->px, 0, (size_t)w * sizeof(*s->px));
  } else if (dy < 0) {
    memmove(s->px, s->px + (size_t)s->pitch, (size_t)(rows - 1) * (size_t)s->pitch * sizeof(*s->px));
    memset(s->px + (size_t)(rows - 1) * (size_t)s->pitch, 0, (size_t)w * sizeof(*s->px));
  }

  /* the noise bar sits in the lower third and drifts; a faint one flickers higher up */
  double t = frame * 0.013;
  int c1 = (int)(rows * (0.74 + 0.05 * sin(t))), h1 = 26 / k;
  int c2 = (int)(rows * (0.22 - 0.04 * sin(t * 1.7))), h2 = 8 / k;
  int band2 = (frame / 5) % 4 != 0;
  int foot = 7 / k;                                  /* head-switching lines at the bottom */

  for (int y = 0; y < rows; y++) {
    uint32_t *row = s->px + (size_t)y * (size_t)s->pitch;
    int d1 = abs(y - c1), d2 = abs(y - c2);
    int a = 0;                                       /* noise strength 0..256 */
    if (d1 < h1) a = 210 - d1 * 170 / h1;
    else if (band2 && d2 < h2) a = 110 - d2 * 90 / h2;
    int off = (int)(sin(y * k * 0.045 + frame * 0.31) * 1.3) + ((xs32(seed) & 15) == 0 ? 1 : 0);
    if (a > 100) off += (int)(xs32(seed) % 23) + a / 16;          /* torn lines in the bar */
    if (y >= rows - foot) off += (y - (rows - foot) + 1) * 14 / foot + (int)(xs32(seed) % 5);
    shift_row(row, tmp, w, off);
    if (!a) continue;
    int n = 0, run = 0;
    for (int x = 0; x < w; x++) {
      uint32_t r = xs32(seed), c = row[x];
      if (run-- <= 0) {                              /* tape noise comes in horizontal streaks */
        n = (r & 7) == 0 ? 230 + (int)(r >> 3 & 25) : (int)(r >> 3 & 0x7f);
        run = 3 + (int)(r >> 10 & 31);
      }
      int cr = (int)(c >> 16 & 0xff), cg = (int)(c >> 8 & 0xff), cb = (int)(c & 0xff);
      cr = (cr * (256 - a) + (n + (int)(r >> 16 & 31) - 16) * a) >> 8;
      cg = (cg * (256 - a) + n * a) >> 8;
      cb = (cb * (256 - a) + (n + (int)(r >> 21 & 31) - 16) * a) >> 8;
      cr = cr < 0 ? 0 : cr > 255 ? 255 : cr;
      cb = cb < 0 ? 0 : cb > 255 ? 255 : cb;
      row[x] = OSD_RGB(cr, cg > 255 ? 255 : cg, cb);
    }
  }
}
