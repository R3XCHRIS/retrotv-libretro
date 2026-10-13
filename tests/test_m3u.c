/* Host unit tests for the M3U parser. Build: make test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/m3u.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)
#define STREQ(a, b) CHECK((a) && !strcmp((a), (b)))

static void test_dispatcharr_style(void)
{
  const char *txt =
    "\xEF\xBB\xBF#EXTM3U x-tvg-url=\"http://h:9191/output/epg/43\" url-tvg=\"http://other\"\r\n"
    "#EXTINF:-1 tvg-id=\"0\" tvg-name=\"RexToons\" tvg-logo=\"http://h/logo/1\" tvg-chno=\"0\" group-title=\"\xF0\x9F\x8E\xA8" "Cartoons\",RexToons\r\n"
    "http://h:9191/proxy/ts/stream/aaa\r\n"
    "#EXTINF:-1 tvg-id=\"12.1\" tvg-name=\"Gilligans Island\" tvg-chno=\"12.1\" group-title=\"Classics, TV\",Gilligan's Island, Season 1\n"
    "http://h:9191/proxy/ts/stream/bbb\n";
  m3u_list l;
  CHECK(m3u_parse(txt, NULL, &l) == 0);
  CHECK(l.count == 2);
  STREQ(l.epg_url, "http://h:9191/output/epg/43");
  STREQ(l.ch[0].name, "RexToons");
  STREQ(l.ch[0].number, "0");
  STREQ(l.ch[0].url, "http://h:9191/proxy/ts/stream/aaa");
  STREQ(l.ch[0].logo, "http://h/logo/1");
  STREQ(l.ch[1].number, "12.1");
  STREQ(l.ch[1].group, "Classics, TV");                 /* comma inside quotes */
  STREQ(l.ch[1].name, "Gilligan's Island, Season 1");   /* title keeps its commas */
  char *g = m3u_ascii(l.ch[0].group);
  STREQ(g, "Cartoons");                                  /* emoji stripped for the 8x8 font */
  free(g);
  m3u_free(&l);
}

static void test_plain_and_relative(void)
{
  const char *txt =
    "# a comment\n"
    "../Shows/ep1.mp4\n"
    "/abs/ep2.mkv\n"
    "#EXTINF:-1,Live\n"
    "udp://239.0.0.1:1234\n";
  m3u_list l;
  CHECK(m3u_parse(txt, "/media/nfs/roms/alpha_player/TV", &l) == 0);
  CHECK(l.count == 3);
  STREQ(l.ch[0].url, "/media/nfs/roms/alpha_player/TV/../Shows/ep1.mp4");
  STREQ(l.ch[0].number, "1");
  STREQ(l.ch[1].url, "/abs/ep2.mkv");
  STREQ(l.ch[2].url, "udp://239.0.0.1:1234");
  STREQ(l.ch[2].name, "Live");
  STREQ(l.ch[2].number, "3");
  m3u_free(&l);
}

static void test_edge_cases(void)
{
  m3u_list l;
  CHECK(m3u_parse("", NULL, &l) == 0 && l.count == 0);
  m3u_free(&l);
  /* EXTINF without a following URL is dropped; trailing line without newline kept */
  CHECK(m3u_parse("#EXTM3U\n#EXTINF:-1,Orphan\n#EXTINF:-1 tvg-name=\"T\",\nhttp://x/y", NULL, &l) == 0);
  CHECK(l.count == 1);
  STREQ(l.ch[0].name, "T");                              /* empty title falls back to tvg-name */
  STREQ(l.ch[0].url, "http://x/y");
  m3u_free(&l);
}

int main(void)
{
  test_dispatcharr_style();
  test_plain_and_relative();
  test_edge_cases();
  if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
  puts("test_m3u: all passed");
  return 0;
}
