/* Host unit tests for guide sources: playlist.txt epg= lines, .gz guides and
 * matching a guide from another source by channel name. Build: make test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/epg.h"
#include "../src/gunzip.h"
#include "../src/plconf.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

/* gzip.compress(b"<tv>" + b"hello guide " * 400 + b"</tv>", mtime=0) from Python */
static const unsigned char GZ[] = { 31,139,8,0,0,0,0,0,2,255,237,198,177,13,0,16,0,0,176,87,124,224,1,113,13,65,34,177,224,126,111,24,218,169,105,223,220,235,156,43,180,51,74,13,238,238,238,238,238,238,238,238,238,191,61,197,125,243,3,134,60,32,202,201,18,0,0 };

int main(void)
{
  plconf pc;
  plconf_parse("# comment\n\n  http://server/playlist.m3u  \n# epg=ignored\nepg=http://guides/x.xml.gz\n", "/media/nfs/retrotv", &pc);
  CHECK(!strcmp(pc.playlist, "http://server/playlist.m3u") && !strcmp(pc.epg, "http://guides/x.xml.gz"));
  plconf_parse("EPG: guide.xml\r\nchannels.m3u\r\n", "/media/nfs/retrotv", &pc);   /* any order, relative, CRLF */
  CHECK(!strcmp(pc.playlist, "/media/nfs/retrotv/channels.m3u") && !strcmp(pc.epg, "/media/nfs/retrotv/guide.xml"));
  plconf_parse("http://a/m3u\n", NULL, &pc);
  CHECK(!strcmp(pc.playlist, "http://a/m3u") && !*pc.epg);
  plconf_parse("# only comments\nepg=/x.xml\n", "/d", &pc);                       /* no playlist yet */
  CHECK(!*pc.playlist && !strcmp(pc.epg, "/x.xml"));
  plconf_parse(NULL, "/d", &pc);
  CHECK(!*pc.playlist && !*pc.epg);

  char err[96] = "";
  size_t n = 0;
  CHECK(gunzip_is_gzip(GZ, sizeof(GZ)) && !gunzip_is_gzip("<tv>", 4));
  char *x = gunzip(GZ, sizeof(GZ), 1 << 20, &n, err, sizeof(err));
  CHECK(x && n == 4 + 12 * 400 + 5 && !strncmp(x, "<tv>hello guide", 15) && !strcmp(x + n - 5, "</tv>"));
  free(x);
  CHECK(!gunzip(GZ, sizeof(GZ) - 10, 1 << 20, &n, err, sizeof(err)) && strstr(err, "truncated"));
  CHECK(!gunzip(GZ, sizeof(GZ), 100, &n, err, sizeof(err)) && strstr(err, "too big"));      /* size cap */
  CHECK(!gunzip("not gzip at all", 15, 1 << 20, &n, err, sizeof(err)));

  /* a guide from another source: its ids don't match the playlist's tvg-id */
  const char *xml =
    "<tv><channel id=\"BBCOne.uk\"><display-name lang=\"en\">BBC One HD</display-name><display-name>BBC1</display-name></channel>"
    "<channel id=\"blackadder.tv\"><display-name>Blackadder</display-name></channel>"
    "<programme start=\"20261013190000 +0000\" stop=\"20261013200000 +0000\" channel=\"BBCOne.uk\"><title>News</title></programme>"
    "<programme start=\"20261013190000 +0000\" stop=\"20261013200000 +0000\" channel=\"blackadder.tv\"><title>Bells</title></programme>"
    "<programme start=\"20261013190000 +0000\" stop=\"20261013200000 +0000\" channel=\"2\"><title>By number</title></programme></tv>";
  time_t t = epg_time("20261013193000 +0000");
  epg *g = epg_parse(xml, t - 7200, t + 7200);
  CHECK(g && g->nalias == 3);
  const epg_chan *c;
  CHECK((c = epg_lookup(g, "zz", "99", NULL, "BBC One")) && !strcmp(c->p[0].title, "News"));   /* name, HD and spaces ignored */
  CHECK((c = epg_lookup(g, NULL, NULL, "bbc1", NULL)) && !strcmp(c->p[0].title, "News"));      /* tvg-name, other display name */
  CHECK((c = epg_lookup(g, "", "2", NULL, "Blackadder")) && !strcmp(c->p[0].title, "Bells"));    /* name before number */
  CHECK((c = epg_lookup(g, "", "2", NULL, "Channel 2")) && !strcmp(c->p[0].title, "By number")); /* own guide: number ok */
  g->match_numbers = 0;                                                                          /* epg= guide */
  CHECK(!epg_lookup(g, "", "2", NULL, "Channel 2"));                                             /* no false match */
  CHECK((c = epg_lookup(g, "", "2", NULL, "Blackadder")) && !strcmp(c->p[0].title, "Bells"));
  g->match_numbers = 1;
  CHECK((c = epg_lookup(g, "blackadder.tv", "2", NULL, NULL)) && !strcmp(c->p[0].title, "Bells")); /* tvg-id first */
  CHECK(!epg_lookup(g, "x", "98", "Nope", "Unknown") && !epg_lookup(NULL, "a", "b", "c", "d"));
  epg_free(g);

  if (fails) return 1;
  printf("test_epgsrc: all passed\n");
  return 0;
}
