/* Host unit tests for remembering the last channel. Build: make test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/lastch.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

int main(void)
{
  m3u_list a = { 0 }, b = { 0 };
  CHECK(!m3u_parse("#EXTM3U\n"
                   "#EXTINF:-1 tvg-chno=\"2\",Blackadder\nhttp://x/1\n"
                   "#EXTINF:-1 tvg-chno=\"14\",Happy Days\nhttp://x/2\n"
                   "#EXTINF:-1 tvg-chno=\"19\",MASH\nhttp://x/3\n", NULL, &a));
  char text[256];
  lastch_format(&a.ch[1], text, sizeof(text));
  CHECK(!strcmp(text, "number=14\nname=Happy Days\n"));
  CHECK(lastch_find(&a, text) == 1);

  /* reordered and renumbered playlist: found by name */
  CHECK(!m3u_parse("#EXTM3U\n"
                   "#EXTINF:-1 tvg-chno=\"5\",MASH\nhttp://x/3\n"
                   "#EXTINF:-1 tvg-chno=\"6\",Happy Days\nhttp://x/2\n", NULL, &b));
  CHECK(lastch_find(&b, text) == 1);
  CHECK(lastch_find(&b, "number=5\nname=Renamed\n") == 0);   /* renamed: by number */
  CHECK(lastch_find(&b, "number=99\nname=Gone\n") == -1);
  CHECK(lastch_find(&b, "garbage") == -1);
  CHECK(lastch_find(&b, "number=\r\nname=MASH\r\n") == 0);   /* CRLF tolerated */
  CHECK(lastch_hash("http://a/m3u") != lastch_hash("http://b/m3u"));
  CHECK(lastch_hash("x") == lastch_hash("x"));

  /* per-channel zoom */
  chanpref cp = { 0 };
  CHECK(chanpref_get(&cp, &a.ch[0]) == -1);
  CHECK(!chanpref_set(&cp, &a.ch[0], 1) && !chanpref_set(&cp, &a.ch[2], 0) && !chanpref_set(&cp, &a.ch[0], 1));
  CHECK(cp.n == 2 && chanpref_get(&cp, &a.ch[0]) == 1 && chanpref_get(&cp, &a.ch[2]) == 0);
  char *txt = chanpref_format(&cp);
  CHECK(txt && !strcmp(txt, "2\tBlackadder\t1\n19\tMASH\t0\n"));
  chanpref back = { 0 };
  chanpref_parse(&back, txt);
  chanpref_parse(&back, "junk\n\tno\tx\n14\tHappy Days\t1");   /* bad lines ignored, no final newline ok */
  CHECK(back.n == 3 && chanpref_get(&back, &a.ch[0]) == 1 && chanpref_get(&back, &a.ch[1]) == 1);
  free(txt);
  chanpref_free(&cp);
  chanpref_free(&back);

  m3u_free(&a);
  m3u_free(&b);
  if (fails) return 1;
  printf("test_lastch: all passed\n");
  return 0;
}
