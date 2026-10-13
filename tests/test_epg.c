/* Host unit tests for the XMLTV guide parser. Build: make test */
#include <stdio.h>
#include <string.h>

#include "../src/epg.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static const char *XML =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tv>\n"
  "  <channel id=\"2\"><display-name>Blackadder</display-name></channel>\n"
  /* out of order on purpose; one with a +0100 offset */
  "  <programme start=\"20261013194027 +0000\" stop=\"20261013201037 +0000\" channel=\"2\">\n"
  "    <title>Bells - S01E02</title><desc>Comedy \xc2\xb7 1983 \xe2\x80\x94 Edmund&#x27;s wedding &amp; more.</desc>\n"
  "  </programme>\n"
  "  <programme start=\"20261013200635 +0100\" stop=\"20261013204027 +0100\" channel=\"2\">\n"
  "    <title lang=\"en\">Potato</title><sub-title>S02E03</sub-title>\n"
  "  </programme>\n"
  "  <programme start=\"20261013201037 +0000\" stop=\"20261013204000 +0000\" channel=\"12.1\">\n"
  "    <title>&quot;Gilligan&quot; &lt;rerun&gt;</title>\n"
  "  </programme>\n"
  "  <programme start=\"20261020000000 +0000\" stop=\"20261020010000 +0000\" channel=\"2\">\n"
  "    <title>Next week</title>\n"
  "  </programme>\n"
  "  <programme start=\"bad\" stop=\"20261013204000 +0000\" channel=\"2\"><title>Broken</title></programme>\n"
  "  <programme start=\"20261013210000 +0000\" stop=\"20261013220000 +0000\" channel=\"2\"><title/></programme>\n"
  "</tv>\n";

int main(void)
{
  /* times: reference values from Python's calendar.timegm */
  time_t t0 = epg_time("20261013190635 +0000");
  CHECK(t0 == 1791918395);
  CHECK(epg_time("20261013200635 +0100") == t0);          /* offset applied */
  CHECK(epg_time("202610131906 -0030") == t0 - 35 + 1800);  /* no seconds, negative offset */
  CHECK(epg_time("bad") == -1 && epg_time("20261399000000 +0000") == -1 && epg_time(NULL) == -1);

  /* keep a window: 19:00 .. 21:00 UTC on the 13th */
  time_t from = epg_time("20261013190000 +0000"), to = epg_time("20261013210000 +0000");
  epg *g = epg_parse(XML, from, to);
  CHECK(g != NULL);
  if (!g) return 1;
  CHECK(g->nch == 2 && g->nprog == 3);                     /* next week, broken, empty title dropped */

  const epg_chan *c = epg_find(g, "2");
  CHECK(c && c->n == 2);
  if (c) {
    CHECK(!strcmp(c->p[0].title, "Potato") && c->p[0].sub && !strcmp(c->p[0].sub, "S02E03"));   /* sorted */
    CHECK(!strcmp(c->p[1].title, "Bells - S01E02"));
    char a[128];
    epg_ascii(c->p[1].desc, a, sizeof(a));
    CHECK(!strcmp(a, "Comedy - 1983 - Edmund's wedding & more."));
    const epg_prog *now, *next;
    epg_now_next(c, epg_time("20261013193000 +0000"), &now, &next);
    CHECK(now == &c->p[0] && next == &c->p[1]);
    epg_now_next(c, epg_time("20261013195000 +0000"), &now, &next);   /* last one on air */
    CHECK(now == &c->p[1] && next == NULL);
    epg_now_next(c, epg_time("20261013203000 +0000"), &now, &next);   /* after the guide ends */
    CHECK(now == NULL && next == NULL);
    epg_now_next(c, epg_time("20261013185000 +0000"), &now, &next);   /* before anything */
    CHECK(now == NULL && next == &c->p[0]);
  }
  const epg_chan *g12 = epg_find(g, "12.1");
  CHECK(g12 && g12->n == 1 && !strcmp(g12->p[0].title, "\"Gilligan\" <rerun>"));
  CHECK(epg_find(g, "99") == NULL && epg_find(NULL, "2") == NULL);
  epg_free(g);

  char a[32];
  epg_ascii("caf\xc3\xa9 \xe2\x80\x9cquoted\xe2\x80\x9d\xe2\x80\xa6 \xf0\x9f\x93\xba", a, sizeof(a));
  CHECK(!strcmp(a, "cafe \"quoted\"... "));               /* emoji dropped */
  epg_ascii("truncate me please", a, 9);
  CHECK(!strcmp(a, "truncate"));

  g = epg_parse("not xml at all", from, to);              /* garbage: empty guide, no crash */
  CHECK(g && g->nch == 0);
  epg_free(g);
  g = epg_parse("<programme start=\"20261013190000\" stop=\"2026", from, to);   /* truncated */
  CHECK(g && g->nprog == 0);
  epg_free(g);

  if (fails) return 1;
  printf("test_epg: all passed\n");
  return 0;
}
