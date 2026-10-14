/* Host unit tests for bar detection and the zoom crop. Build: make test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/aspect.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

enum { W = 640, H = 480 };
static uint8_t img[W * H];

/* black frame with a mid-grey picture inside the given bars */
static void make(int top, int left)
{
  memset(img, 16, sizeof(img));
  for (int y = top; y < H - top; y++)
    memset(img + (size_t)y * W + left, 120, (size_t)(W - 2 * left));
}

int main(void)
{
  int t, l, x, y, cw, ch;

  make(60, 0);                                   /* 16:9 letterboxed in 4:3 */
  CHECK(aspect_bars(img, W, W, H, &t, &l) && t == 60 && l == 0);
  make(0, 80);                                   /* 4:3 pillarboxed in a wider frame */
  CHECK(aspect_bars(img, W, W, H, &t, &l) && t == 0 && l == 80);
  make(0, 0);
  CHECK(aspect_bars(img, W, W, H, &t, &l) && t == 0 && l == 0);
  memset(img, 16, sizeof(img));                  /* all black: can't tell */
  CHECK(!aspect_bars(img, W, W, H, &t, &l));
  make(60, 0);                                   /* a caption in the bottom bar: smaller bar wins */
  memset(img + (size_t)(H - 30) * W + 200, 200, 100);
  CHECK(aspect_bars(img, W, W, H, &t, &l) && t == 29);

  aspect_tracker tr;
  aspect_track_reset(&tr);
  aspect_track(&tr, 60, 0);
  CHECK(tr.top == 0);                            /* one sample isn't enough */
  aspect_track(&tr, 61, 0);
  aspect_track(&tr, 60, 0);
  aspect_track(&tr, 59, 0);
  CHECK(tr.top == 59 || tr.top == 60);
  aspect_track(&tr, 0, 0);                       /* a single odd frame doesn't undo it */
  CHECK(tr.top >= 59);

  aspect_zoom_rect(W, H, 1.0, 60, 0, &x, &y, &cw, &ch);   /* letterbox -> 480x360 centre */
  CHECK(cw == 480 && ch == 360 && x == 80 && y == 60);
  aspect_zoom_rect(1920, 1080, 1.0, 0, 0, &x, &y, &cw, &ch);
  CHECK(cw == 1440 && ch == 1080 && x == 240 && y == 0);
  aspect_zoom_rect(W, H, 1.0, 0, 0, &x, &y, &cw, &ch);     /* already 4:3: unchanged */
  CHECK(cw == 640 && ch == 480 && x == 0 && y == 0);
  aspect_zoom_rect(1280, 720, 405.0 / 539.0, 0, 0, &x, &y, &cw, &ch);  /* anamorphic 4:3 */
  CHECK(cw >= 1270 && ch >= 716);
  aspect_zoom_rect(1920, 1080, 1.0, 0, 240, &x, &y, &cw, &ch);  /* 4:3 pillarboxed in 16:9 */
  CHECK(cw == 1440 && ch == 1080 && x == 240);

  if (fails) return 1;
  printf("test_aspect: all passed\n");
  return 0;
}
