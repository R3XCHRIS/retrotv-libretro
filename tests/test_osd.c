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
  osd_surface s = { px, W, H, W };

  memset(px, 0, sizeof(px));
  osd_fill(&s, -10, -10, 1000, 1000, OSD_RGB(255, 0, 0), 255);          /* clipped, no crash */
  CHECK(px[0] == OSD_RGB(255, 0, 0) && px[W * H - 1] == OSD_RGB(255, 0, 0));

  memset(px, 0, sizeof(px));
  osd_fill(&s, 0, 0, W, H, OSD_RGB(200, 100, 50), 128);                 /* ~50% over black */
  uint32_t c = px[5];
  CHECK(((c >> 16) & 0xff) >= 99 && ((c >> 16) & 0xff) <= 101);
  CHECK(((c >> 8) & 0xff) >= 49 && ((c >> 8) & 0xff) <= 51);

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
