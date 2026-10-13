/* osd.h - tiny software overlay for an XRGB8888 framebuffer (8x8 bitmap font). */
#ifndef RETROTV_OSD_H
#define RETROTV_OSD_H

#include <stdint.h>

typedef struct {
  uint32_t *px;   /* XRGB8888 */
  int w, h;       /* pixels */
  int pitch;      /* pixels per row */
} osd_surface;

#define OSD_RGB(r, g, b) ((uint32_t)(((r) & 0xff) << 16 | ((g) & 0xff) << 8 | ((b) & 0xff)))

/* Fill a rectangle; alpha 0..255 blends over existing pixels. Clipped. */
void osd_fill(osd_surface *s, int x, int y, int w, int h, uint32_t rgb, int alpha);
/* Draw ASCII text at integer scale; non-ASCII becomes '?'. Returns width drawn. */
int osd_text(osd_surface *s, int x, int y, const char *text, int scale, uint32_t rgb);
/* Same, with a 1px-per-scale drop shadow for readability over video. */
int osd_text_shadow(osd_surface *s, int x, int y, const char *text, int scale, uint32_t rgb);
int osd_text_width(const char *text, int scale);
/* TV static: random grey noise. seed is advanced. */
void osd_static(osd_surface *s, uint32_t *seed);

#endif
