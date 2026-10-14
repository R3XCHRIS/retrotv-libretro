/* Host unit tests for the TV guide layout. Build: make test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/guide.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

int main(void)
{
  /* channel 2: 19:00-19:30, 19:30-20:15, 20:15-21:00 (UTC) */
  const char *xml =
    "<programme start=\"20261013190000 +0000\" stop=\"20261013193000 +0000\" channel=\"2\"><title>A</title></programme>"
    "<programme start=\"20261013193000 +0000\" stop=\"20261013201500 +0000\" channel=\"2\"><title>B</title><desc>Long text here</desc></programme>"
    "<programme start=\"20261013201500 +0000\" stop=\"20261013210000 +0000\" channel=\"2\"><title>C</title></programme>";
  time_t t1900 = epg_time("20261013190000 +0000");
  epg *g = epg_parse(xml, t1900 - 7200, t1900 + 26 * 3600);
  CHECK(g && g->nprog == 3);
  const epg_chan *c = epg_find(g, "2");

  /* window 19:15 .. 20:45 across 450 px: A clipped at the left, C clipped at the right */
  guide_cell cells[8];
  int n = guide_cells(c, t1900 + 15 * 60, GUIDE_SPAN, 100, 450, cells, 8);
  CHECK(n == 3);
  CHECK(cells[0].x == 100 && cells[0].w == 75 && !strcmp(cells[0].p->title, "A"));   /* 15 min = 75 px */
  CHECK(cells[1].x == 175 && cells[1].w == 225);                                      /* 45 min */
  CHECK(cells[2].x == 400 && cells[2].w == 150);                                      /* 30 min, to the edge */
  CHECK(guide_cells(c, t1900 + 3 * 3600, GUIDE_SPAN, 100, 450, cells, 8) == 0);       /* nothing then */
  CHECK(guide_cells(NULL, t1900, GUIDE_SPAN, 100, 450, cells, 8) == 0);
  CHECK(guide_cells(c, t1900, GUIDE_SPAN, 100, 450, cells, 1) == 1);                  /* respects max */

  /* scrolling keeps the selection on screen */
  guide_view v = { 0, 0, 0 };
  v.sel = 9; guide_scroll(&v, 20);
  CHECK(v.top == 9 - GUIDE_ROWS + 1);
  v.sel = 2; guide_scroll(&v, 20);
  CHECK(v.top == 2);
  v.sel = 19; v.top = 18; guide_scroll(&v, 20);
  CHECK(v.top == 20 - GUIDE_ROWS);
  v.sel = 3; v.top = 1; guide_scroll(&v, 4);                                          /* short list */
  CHECK(v.top <= v.sel && v.sel < v.top + GUIDE_ROWS);

  /* focus: programme at max(now, t0) on the highlighted channel */
  m3u_list l = { 0 };
  CHECK(!m3u_parse("#EXTM3U\n#EXTINF:-1 tvg-id=\"2\" tvg-chno=\"2\",Blackadder\nhttp://x/1\n"
                   "#EXTINF:-1 tvg-chno=\"7\",No Guide\nhttp://x/2\n", NULL, &l));
  v = (guide_view){ 0, 0, t1900 };
  CHECK(guide_focus(g, &l, &v, t1900 + 40 * 60) == &c->p[1]);                         /* now = 19:40 -> B */
  v.t0 = t1900 + 3600;                                                                 /* paged ahead to 20:00 */
  CHECK(guide_focus(g, &l, &v, t1900 + 40 * 60) == &c->p[1]);                         /* B still on at 20:00 */
  v.t0 = t1900 + 90 * 60;
  CHECK(guide_focus(g, &l, &v, t1900) == &c->p[2]);
  v.t0 = t1900 + 10 * 60;                                                              /* A (ends 19:30) has 20 min left: kept */
  CHECK(guide_focus(g, &l, &v, t1900) == &c->p[0]);
  v.t0 = t1900 + 25 * 60;                                                              /* only 5 min left: next one */
  CHECK(guide_focus(g, &l, &v, t1900) == &c->p[1]);
  v.sel = 1;
  CHECK(guide_focus(g, &l, &v, t1900) == NULL);                                        /* no guide data */

  /* drawing the whole screen: no crash, preview copied, ruler drawn */
  uint32_t *px = calloc(640 * 480, 4);
  for (int i = 0; i < 640 * 480; i++) px[i] = 0x00ff0000;                              /* red "video" */
  osd_surface s = { px, 640, 480, 640 };
  v = (guide_view){ 0, 0, t1900 };
  guide_draw(&s, &l, g, &v, 0, t1900 + 40 * 60);
  CHECK(px[(24 + 60) * 640 + 32 + 80] == 0x00ff0000);                                 /* preview shows the picture */
  CHECK(px[2 * 640 + 2] != 0x00ff0000);                                                /* rest covered */
  guide_draw(&s, &l, NULL, &v, 0, t1900);                                              /* no guide yet */
  free(px);

  m3u_free(&l);
  epg_free(g);
  if (fails) return 1;
  printf("test_guide: all passed\n");
  return 0;
}
