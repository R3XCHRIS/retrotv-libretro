#include "epg.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DESC_MAX 400            /* bytes kept of a description */

/* ---------- time ---------- */

/* days since 1970-01-01 for a civil date (Howard Hinnant's algorithm) */
static long days_from_civil(long y, unsigned m, unsigned d)
{
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

static int digits(const char *s, int n, long *out)
{
  long v = 0;
  for (int i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return -1;
    v = v * 10 + (s[i] - '0');
  }
  *out = v;
  return 0;
}

time_t epg_time(const char *s)
{
  long Y, M, D, h, m, sec = 0, oh, om;
  if (!s || digits(s, 4, &Y) || digits(s + 4, 2, &M) || digits(s + 6, 2, &D)
      || digits(s + 8, 2, &h) || digits(s + 10, 2, &m)) return -1;
  const char *p = s + 12;
  if (p[0] >= '0' && p[0] <= '9') { if (digits(p, 2, &sec)) return -1; p += 2; }
  if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || sec > 60) return -1;
  long t = days_from_civil(Y, (unsigned)M, (unsigned)D) * 86400L + h * 3600 + m * 60 + sec;
  while (*p == ' ') p++;
  if ((*p == '+' || *p == '-') && !digits(p + 1, 2, &oh) && !digits(p + 3, 2, &om))
    t -= (*p == '+' ? 1 : -1) * (oh * 3600 + om * 60);   /* local = UTC + offset */
  return (time_t)t;
}

/* ---------- text ---------- */

static void put_utf8(char **o, char *end, unsigned long cp)
{
  char b[4];
  int n;
  if (cp < 0x80) { b[0] = (char)cp; n = 1; }
  else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
  else if (cp < 0x10000) { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
  else { b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
  if (*o + n > end) return;
  memcpy(*o, b, (size_t)n);
  *o += n;
}

/* Copy [s, e) decoding XML entities and folding whitespace; at most max bytes. */
static char *xml_text(const char *s, const char *e, size_t max)
{
  size_t cap = (size_t)(e - s) < max ? (size_t)(e - s) : max;
  char *out = malloc(cap + 1), *o = out, *end = out + cap;
  if (!out) return NULL;
  int space = 0;
  while (s < e && o < end) {
    unsigned long cp = 0;
    if (*s == '&') {
      const char *semi = memchr(s, ';', (size_t)(e - s) < 12 ? (size_t)(e - s) : 12);
      if (semi) {
        size_t l = (size_t)(semi - s - 1);
        const char *nm = s + 1;
        if (l == 3 && !memcmp(nm, "amp", 3)) cp = '&';
        else if (l == 2 && !memcmp(nm, "lt", 2)) cp = '<';
        else if (l == 2 && !memcmp(nm, "gt", 2)) cp = '>';
        else if (l == 4 && !memcmp(nm, "quot", 4)) cp = '"';
        else if (l == 4 && !memcmp(nm, "apos", 4)) cp = '\'';
        else if (l > 1 && nm[0] == '#') cp = strtoul(nm[1] == 'x' || nm[1] == 'X' ? nm + 2 : nm + 1, NULL, nm[1] == 'x' || nm[1] == 'X' ? 16 : 10);
        if (cp) {
          if (space && o > out) *o++ = ' ';
          space = 0;
          put_utf8(&o, end, cp);
          s = semi + 1;
          continue;
        }
      }
    }
    if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') { space = 1; s++; continue; }
    if (space && o > out && o < end) *o++ = ' ';
    space = 0;
    if (o < end) *o++ = *s;
    s++;
  }
  *o = '\0';
  return out;
}

void epg_ascii(const char *in, char *out, size_t n)
{
  /* Latin-1 letters U+00C0..U+00FF without their accents */
  static const char lat[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
  size_t o = 0;
  if (!n) return;
  for (const unsigned char *s = (const unsigned char *)in; *s && o + 1 < n;) {
    unsigned long cp;
    int len = *s < 0x80 ? 1 : (*s >> 5) == 6 ? 2 : (*s >> 4) == 14 ? 3 : (*s >> 3) == 30 ? 4 : 1;
    cp = len == 1 ? *s : (unsigned long)(*s & (0x7F >> len));
    for (int i = 1; i < len; i++) {
      if ((s[i] & 0xC0) != 0x80) { len = i; cp = '?'; break; }
      cp = (cp << 6) | (s[i] & 0x3F);
    }
    s += len;
    const char *rep = NULL;
    char one[2] = { 0, 0 };
    if (cp >= 0x20 && cp < 0x7F) one[0] = (char)cp;
    else if (cp == '\t' || cp == '\n' || cp == 0xA0) one[0] = ' ';
    else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201B || cp == 0x2032 || cp == 0xB4) one[0] = '\'';
    else if (cp == 0x201C || cp == 0x201D || cp == 0x2033) one[0] = '"';
    else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212 || cp == 0xB7 || cp == 0x2022) one[0] = '-';
    else if (cp == 0x2026) rep = "...";
    else if (cp >= 0xC0 && cp <= 0xFF) one[0] = lat[cp - 0xC0];
    if (!rep) rep = one;
    for (; *rep && o + 1 < n; rep++) out[o++] = *rep;
  }
  out[o] = '\0';
}

/* Shorten a title to max characters: drop a trailing " - S01E02" first, then
 * cut and remove any dangling separator so it never ends in " -". */
void epg_shorten(char *t, int max)
{
  if (max < 1) { t[0] = '\0'; return; }
  if ((int)strlen(t) > max) {
    char *d = strstr(t, " - S");
    if (d && d[4] >= '0' && d[4] <= '9') *d = '\0';
  }
  if ((int)strlen(t) > max) {
    int cut = max;
    if (t[max] != ' ') {                           /* mid-word: back off to a nearby space */
      int sp = max;
      while (sp > 0 && t[sp] != ' ') sp--;
      if (sp >= max * 3 / 5) cut = sp;
    }
    t[cut] = '\0';
  }
  size_t l = strlen(t);
  while (l && (t[l - 1] == ' ' || t[l - 1] == '-' || t[l - 1] == ':' || t[l - 1] == ',')) t[--l] = '\0';
}

/* ---------- parsing ---------- */

typedef struct { char *chan; epg_prog p; } item;

/* value of attr="..." inside [tag, end) copied into out */
static int attr(const char *tag, const char *end, const char *name, char *out, size_t n)
{
  size_t nl = strlen(name);
  for (const char *p = tag + 1; p + nl + 2 < end; p++) {   /* tag[0] is '<' */
    if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n') && !memcmp(p, name, nl) && p[nl] == '=' && (p[nl + 1] == '"' || p[nl + 1] == '\'')) {
      char q = p[nl + 1];
      const char *v = p + nl + 2, *e = memchr(v, q, (size_t)(end - v));
      if (!e) return -1;
      size_t l = (size_t)(e - v) < n - 1 ? (size_t)(e - v) : n - 1;
      memcpy(out, v, l);
      out[l] = '\0';
      return 0;
    }
  }
  return -1;
}

/* text of the first <name ...>...</name> in [s, e) */
static char *child(const char *s, const char *e, const char *name, size_t max)
{
  char open[32], close[32];
  snprintf(open, sizeof(open), "<%s", name);
  snprintf(close, sizeof(close), "</%s>", name);
  size_t ol = strlen(open);
  for (const char *p = s; p < e; p++) {
    p = memchr(p, '<', (size_t)(e - p));
    if (!p) return NULL;
    if (!strncmp(p, open, ol) && (p[ol] == '>' || p[ol] == ' ')) {
      const char *gt = memchr(p, '>', (size_t)(e - p));
      if (!gt || gt[-1] == '/') return NULL;
      const char *c = strstr(gt + 1, close);
      if (!c || c > e) return NULL;
      return xml_text(gt + 1, c, max);
    }
  }
  return NULL;
}

static int cmp_item(const void *a, const void *b)
{
  const item *x = a, *y = b;
  int c = strcmp(x->chan, y->chan);
  return c ? c : (x->p.start > y->p.start) - (x->p.start < y->p.start);
}

static int cmp_chan(const void *key, const void *c) { return strcmp(key, ((const epg_chan *)c)->id); }

static void free_prog(epg_prog *p) { free(p->title); free(p->sub); free(p->desc); }

epg *epg_parse(const char *xml, time_t from, time_t to)
{
  epg *g = calloc(1, sizeof(*g));
  item *it = NULL;
  size_t n = 0, cap = 0;
  if (!g) return NULL;
  for (const char *p = xml ? strstr(xml, "<programme") : NULL; p; p = strstr(p + 1, "<programme")) {
    const char *gt = strchr(p, '>'), *end = gt ? strstr(gt, "</programme>") : NULL;
    if (!end) break;
    char start[32], stop[32], chan[128];
    if (attr(p, gt, "start", start, sizeof(start)) || attr(p, gt, "stop", stop, sizeof(stop))
        || attr(p, gt, "channel", chan, sizeof(chan))) continue;
    time_t t0 = epg_time(start), t1 = epg_time(stop);
    if (t0 < 0 || t1 <= t0 || t1 <= from || t0 >= to) continue;
    char *title = child(gt + 1, end, "title", 256);
    if (!title) continue;
    if (n == cap) {
      size_t nc = cap ? cap * 2 : 256;
      item *ni = realloc(it, nc * sizeof(*ni));
      if (!ni) { free(title); goto fail; }
      it = ni;
      cap = nc;
    }
    item *x = &it[n];
    x->chan = strdup(chan);
    x->p = (epg_prog){ t0, t1, title, child(gt + 1, end, "sub-title", 256), child(gt + 1, end, "desc", DESC_MAX) };
    if (!x->chan) { free_prog(&x->p); goto fail; }
    n++;
    p = end;
  }
  qsort(it, n, sizeof(*it), cmp_item);
  for (size_t i = 0; i < n;) {                      /* group into channels */
    size_t j = i;
    while (j < n && !strcmp(it[j].chan, it[i].chan)) j++;
    epg_chan *nc = realloc(g->ch, (g->nch + 1) * sizeof(*nc));
    epg_prog *ps = malloc((j - i) * sizeof(*ps));
    if (!nc || !ps) { if (nc) g->ch = nc; free(ps); goto fail; }
    g->ch = nc;
    epg_chan *c = &g->ch[g->nch++];
    c->id = it[i].chan;
    c->p = ps;
    c->n = j - i;
    for (size_t k = i; k < j; k++) {
      ps[k - i] = it[k].p;
      it[k].p.title = it[k].p.sub = it[k].p.desc = NULL;   /* moved */
      if (k > i) { free(it[k].chan); it[k].chan = NULL; }
    }
    it[i].chan = NULL;                                       /* now owned by the channel */
    g->nprog += j - i;
    i = j;
  }
  free(it);
  return g;
fail:
  for (size_t k = 0; k < n; k++) { free(it[k].chan); free_prog(&it[k].p); }
  free(it);
  epg_free(g);
  return NULL;
}

void epg_free(epg *g)
{
  if (!g) return;
  for (size_t i = 0; i < g->nch; i++) {
    for (size_t k = 0; k < g->ch[i].n; k++) free_prog(&g->ch[i].p[k]);
    free(g->ch[i].p);
    free(g->ch[i].id);
  }
  free(g->ch);
  free(g);
}

const epg_chan *epg_find(const epg *g, const char *id)
{
  if (!g || !id || !g->nch) return NULL;
  return bsearch(id, g->ch, g->nch, sizeof(*g->ch), cmp_chan);
}

void epg_now_next(const epg_chan *c, time_t t, const epg_prog **now, const epg_prog **next)
{
  *now = *next = NULL;
  if (!c) return;
  for (size_t i = 0; i < c->n; i++) {
    if (c->p[i].start <= t && t < c->p[i].stop) { *now = &c->p[i]; continue; }
    if (c->p[i].start >= t) { *next = &c->p[i]; return; }
  }
}
