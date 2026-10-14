/* Host unit tests for the overlay renderer. Build: make test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/osd.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

int main(void)
{
  enum { W = 64, H = 32 };
  uint32_t px[W * H];
  osd_surface s = { px, W, H, W, 0 };

  memset(px, 0, sizeof(px));
  osd_fill(&s, -10, -10, 1000, 1000, OSD_RGB(255, 0, 0), 255);          /* clipped, no crash */
  CHECK(px[0] == OSD_RGB(255, 0, 0) && px[W * H - 1] == OSD_RGB(255, 0, 0));

  memset(px, 0, sizeof(px));
  osd_fill(&s, 0, 0, W, H, OSD_RGB(200, 100, 50), 128);                 /* ~50% over black */
  uint32_t c = px[5];
  CHECK(((c >> 16) & 0xff) >= 99 && ((c >> 16) & 0xff) <= 101);
  CHECK(((c >> 8) & 0xff) >= 49 && ((c >> 8) & 0xff) <= 51);

  /* over a coloured picture: each channel blends on its own (red used to leak into blue) */
  for (int i = 0; i < W * H; i++) px[i] = OSD_RGB(200, 150, 100);
  osd_fill(&s, 0, 0, W, H, OSD_RGB(8, 16, 72), 235);
  c = px[0];
  CHECK(((c >> 16) & 0xff) == (8 * 235 + 200 * 20 + 127) / 255);
  CHECK(((c >> 8) & 0xff) == (16 * 235 + 150 * 20 + 127) / 255);
  CHECK((c & 0xff) == (72 * 235 + 100 * 20 + 127) / 255);
  for (int i = 0; i < W * H; i++) px[i] = OSD_RGB(255, 255, 255);
  osd_fill(&s, 0, 0, W, H, OSD_RGB(255, 0, 0), 128);             /* no channel overflows */
  CHECK(px[0] == OSD_RGB(255, 127, 127));

  /* 240p: logical 640x480 coordinates on a buffer with half the rows */
  {
    enum { LW = 64, LH = 64 };
    static uint32_t half[LW * (LH / 2)];
    osd_surface h = { half, LW, LH, LW, 1 };
    CHECK(osd_rows(&h) == LH / 2 && osd_line_h(&h) == 16);
    memset(half, 0, sizeof(half));
    osd_fill(&h, 0, 20, LW, 10, OSD_RGB(255, 0, 0), 255);         /* logical 20..29 -> rows 10..14 */
    CHECK(half[9 * LW] == 0 && half[10 * LW] && half[14 * LW] && half[15 * LW] == 0);
    memset(half, 0, sizeof(half));
    osd_fill(&h, 0, 41, LW, 1, OSD_RGB(255, 0, 0), 255);          /* a 1-pixel rule stays visible */
    CHECK(half[20 * LW]);
    memset(half, 0, sizeof(half));
    osd_fill(&h, 0, 0, LW, 1000, OSD_RGB(9, 9, 9), 255);         /* clipped to the buffer, no overflow */
    memset(half, 0, sizeof(half));
    osd_text(&h, 0, 16, "I", 1, OSD_RGB(255, 255, 255));          /* 1x keeps 8 rows: rows 8..15 */
    int top = -1, bot = -1;
    for (int r = 0; r < LH / 2; r++) for (int c = 0; c < 8; c++) if (half[r * LW + c]) { if (top < 0) top = r; bot = r; }
    CHECK(top >= 8 && bot <= 15 && bot - top >= 5);
    memset(half, 0, sizeof(half));
    osd_text(&h, 0, 0, "I", 4, OSD_RGB(255, 255, 255));           /* 4x -> 2 rows per glyph row */
    top = bot = -1;
    for (int r = 0; r < LH / 2; r++) for (int c = 0; c < 32; c++) if (half[r * LW + c]) { if (top < 0) top = r; bot = r; }
    CHECK(bot <= 15 && bot - top >= 11);
    uint32_t seed = 1;
    osd_static(&h, &seed);                                            /* stays inside the 32 rows */
    osd_static_vhs(&h, &seed, 7);
  }

  memset(px, 0, sizeof(px));
  CHECK(osd_text_width("AB", 2) == 32);
  int w = osd_text(&s, 0, 0, "I", 1, OSD_RGB(255, 255, 255));
  CHECK(w == 8);
  int lit = 0;
  for (int i = 0; i < W * H; i++) lit += px[i] != 0;
  CHECK(lit > 5 && lit < 64);                                             /* glyph drawn inside its 8x8 cell */
  for (int y = 0; y < H; y++) for (int x = 8; x < W; x++) CHECK(px[y * W + x] == 0);

  osd_text(&s, W - 4, H - 4, "\xF0\x9F\x8E\xA8 overflow", 3, OSD_RGB(1, 2, 3)); /* clipped, non-ASCII safe */

  uint32_t seed = 1, before = seed;
  osd_static(&s, &seed);
  CHECK(seed != before);
  int distinct = 0;
  for (int i = 1; i < W; i++) distinct += px[i] != px[i - 1];
  CHECK(distinct > 4);

  if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
  puts("test_osd: all passed");
  return 0;
}
