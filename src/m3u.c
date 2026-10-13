#include "m3u.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dupn(const char *s, size_t n)
{
  char *d = malloc(n + 1);
  if (!d) return NULL;
  memcpy(d, s, n);
  d[n] = '\0';
  return d;
}

static char *dupz(const char *s) { return s ? dupn(s, strlen(s)) : NULL; }

/* Value of key="value" (or key=value) inside an attribute string, or NULL. */
static char *attr(const char *line, const char *end, const char *key)
{
  size_t klen = strlen(key);
  for (const char *p = line; p + klen < end; p++) {
    if (strncmp(p, key, klen) || p[klen] != '=') continue;
    if (p != line && !isspace((unsigned char)p[-1]) && p[-1] != ':') continue;
    const char *v = p + klen + 1;
    if (*v == '"') {
      const char *q = memchr(v + 1, '"', (size_t)(end - v - 1));
      if (!q) return NULL;
      return dupn(v + 1, (size_t)(q - v - 1));
    }
    const char *q = v;
    while (q < end && !isspace((unsigned char)*q) && *q != ',') q++;
    return dupn(v, (size_t)(q - v));
  }
  return NULL;
}

/* First comma that is not inside double quotes. */
static const char *title_comma(const char *s, const char *end)
{
  int inq = 0;
  for (const char *p = s; p < end; p++) {
    if (*p == '"') inq = !inq;
    else if (*p == ',' && !inq) return p;
  }
  return NULL;
}

static char *trim_dup(const char *s, const char *e)
{
  while (s < e && isspace((unsigned char)*s)) s++;
  while (e > s && isspace((unsigned char)e[-1])) e--;
  return dupn(s, (size_t)(e - s));
}

char *m3u_ascii(const char *s)
{
  if (!s) return dupz("");
  size_t n = strlen(s);
  char *d = malloc(n + 1);
  if (!d) return NULL;
  size_t j = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c >= 0x20 && c < 0x7f) d[j++] = (char)c;
  }
  d[j] = '\0';
  char *t = trim_dup(d, d + j);
  free(d);
  return t;
}

static int is_url(const char *s)
{
  const char *p = strstr(s, "://");
  if (!p || p == s) return 0;
  for (const char *q = s; q < p; q++)
    if (!isalnum((unsigned char)*q) && *q != '+' && *q != '-' && *q != '.') return 0;
  return 1;
}

static int push(m3u_list *l, m3u_channel *c)
{
  m3u_channel *n = realloc(l->ch, (l->count + 1) * sizeof(*n));
  if (!n) return -1;
  l->ch = n;
  l->ch[l->count++] = *c;
  return 0;
}

static void chan_free(m3u_channel *c)
{
  free(c->name); free(c->url); free(c->number); free(c->tvg_id);
  free(c->tvg_name); free(c->logo); free(c->group);
  memset(c, 0, sizeof(*c));
}

int m3u_parse(const char *text, const char *base_dir, m3u_list *out)
{
  memset(out, 0, sizeof(*out));
  m3u_channel pending = {0};
  int have_info = 0;
  const char *p = text;

  while (p && *p) {
    const char *eol = p;
    while (*eol && *eol != '\n' && *eol != '\r') eol++;
    const char *s = p, *e = eol;
    while (s < e && isspace((unsigned char)*s)) s++;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    /* skip UTF-8 BOM on the first line */
    if (s == text && e - s >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;

    if (s < e) {
      if (!strncmp(s, "#EXTM3U", 7)) {
        if (!out->epg_url) out->epg_url = attr(s, e, "x-tvg-url");
        if (!out->epg_url) out->epg_url = attr(s, e, "url-tvg");
      } else if (!strncmp(s, "#EXTINF:", 8)) {
        chan_free(&pending);
        const char *comma = title_comma(s + 8, e);
        const char *attr_end = comma ? comma : e;
        pending.tvg_id   = attr(s + 8, attr_end, "tvg-id");
        pending.tvg_name = attr(s + 8, attr_end, "tvg-name");
        pending.logo     = attr(s + 8, attr_end, "tvg-logo");
        pending.group    = attr(s + 8, attr_end, "group-title");
        pending.number   = attr(s + 8, attr_end, "tvg-chno");
        if (comma) pending.name = trim_dup(comma + 1, e);
        have_info = 1;
      } else if (*s != '#') {
        char *loc = trim_dup(s, e);
        if (!loc) { chan_free(&pending); m3u_free(out); return -1; }
        if (!is_url(loc) && loc[0] != '/' && base_dir && *base_dir) {
          size_t n = strlen(base_dir) + 1 + strlen(loc) + 1;
          char *abs = malloc(n);
          if (!abs) { free(loc); chan_free(&pending); m3u_free(out); return -1; }
          snprintf(abs, n, "%s/%s", base_dir, loc);
          free(loc);
          loc = abs;
        }
        if (!have_info) memset(&pending, 0, sizeof(pending));
        pending.url = loc;
        if (!pending.name || !*pending.name) {
          free(pending.name);
          pending.name = dupz(pending.tvg_name ? pending.tvg_name : loc);
        }
        if (!pending.number) {
          char num[16];
          snprintf(num, sizeof(num), "%zu", out->count + 1);
          pending.number = dupz(num);
        }
        if (push(out, &pending)) { chan_free(&pending); m3u_free(out); return -1; }
        memset(&pending, 0, sizeof(pending));
        have_info = 0;
      }
    }
    p = *eol ? eol + 1 : NULL;
  }
  chan_free(&pending);
  return 0;
}

int m3u_load(const char *path, m3u_list *out)
{
  memset(out, 0, sizeof(*out));
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  size_t cap = 65536, len = 0;
  char *buf = malloc(cap + 1);
  size_t r;
  while (buf && (r = fread(buf + len, 1, cap - len, f)) > 0) {
    len += r;
    if (len == cap) {
      char *nb = realloc(buf, cap * 2 + 1);
      if (!nb) { free(buf); buf = NULL; break; }
      buf = nb; cap *= 2;
    }
  }
  fclose(f);
  if (!buf) return -1;
  buf[len] = '\0';

  char *dir = dupz(path);
  char *slash = dir ? strrchr(dir, '/') : NULL;
  if (slash) *slash = '\0'; else if (dir) { free(dir); dir = dupz("."); }
  int rc = m3u_parse(buf, dir, out);
  free(dir);
  free(buf);
  return rc;
}

void m3u_free(m3u_list *list)
{
  for (size_t i = 0; i < list->count; i++) chan_free(&list->ch[i]);
  free(list->ch);
  free(list->epg_url);
  memset(list, 0, sizeof(*list));
}
