#include "lastch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t lastch_hash(const char *src)
{
  uint32_t h = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)src; *p; p++) { h ^= *p; h *= 16777619u; }
  return h;
}

void lastch_format(const m3u_channel *c, char *out, size_t n)
{
  snprintf(out, n, "number=%s\nname=%s\n", c->number ? c->number : "", c->name ? c->name : "");
}

/* value of "key=" up to the end of its line */
static void field(const char *text, const char *key, char *out, size_t n)
{
  size_t kl = strlen(key);
  out[0] = '\0';
  for (const char *l = text; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
    if (strncmp(l, key, kl) || l[kl] != '=') continue;
    const char *v = l + kl + 1, *e = strpbrk(v, "\r\n");
    size_t len = e ? (size_t)(e - v) : strlen(v);
    if (len >= n) len = n - 1;
    memcpy(out, v, len);
    out[len] = '\0';
    return;
  }
}

static int same(const char *a, const char *b) { return a && b && *b && !strcmp(a, b); }

int lastch_find(const m3u_list *list, const char *text)
{
  char num[64], name[256];
  if (!list || !text) return -1;
  field(text, "number", num, sizeof(num));
  field(text, "name", name, sizeof(name));
  for (size_t i = 0; i < list->count; i++)
    if (same(list->ch[i].number, num) && same(list->ch[i].name, name)) return (int)i;
  for (size_t i = 0; i < list->count; i++)
    if (same(list->ch[i].name, name)) return (int)i;
  for (size_t i = 0; i < list->count; i++)
    if (same(list->ch[i].number, num)) return (int)i;
  return -1;
}

/* ---------- per-channel zoom ---------- */

static char *pref_key(const m3u_channel *c)
{
  const char *num = c->number ? c->number : "", *name = c->name ? c->name : "";
  size_t n = strlen(num) + strlen(name) + 2;
  char *k = malloc(n);
  if (k) snprintf(k, n, "%s\t%s", num, name);
  return k;
}

static long pref_find(const chanpref *p, const char *key)
{
  for (size_t i = 0; i < p->n; i++) if (!strcmp(p->key[i], key)) return (long)i;
  return -1;
}

static int pref_put(chanpref *p, char *key, int zoom)        /* takes key */
{
  long i = pref_find(p, key);
  if (i >= 0) { p->zoom[i] = (unsigned char)(zoom != 0); free(key); return 0; }
  char **nk = realloc(p->key, (p->n + 1) * sizeof(*nk));
  if (!nk) { free(key); return -1; }
  p->key = nk;
  unsigned char *nz = realloc(p->zoom, p->n + 1);
  if (!nz) { free(key); return -1; }
  p->zoom = nz;
  p->key[p->n] = key;
  p->zoom[p->n] = (unsigned char)(zoom != 0);
  p->n++;
  return 0;
}

void chanpref_parse(chanpref *p, const char *text)
{
  for (const char *l = text; l && *l;) {
    const char *e = strchr(l, '\n');
    size_t len = e ? (size_t)(e - l) : strlen(l);
    const char *t2 = NULL;
    for (size_t i = 0, tabs = 0; i < len; i++) if (l[i] == '\t' && ++tabs == 2) { t2 = l + i; break; }
    if (t2 && t2 + 1 < l + len && (t2[1] == '0' || t2[1] == '1')) {
      size_t kl = (size_t)(t2 - l);
      char *k = malloc(kl + 1);
      if (k) { memcpy(k, l, kl); k[kl] = '\0'; pref_put(p, k, t2[1] == '1'); }
    }
    l = e ? e + 1 : NULL;
  }
}

int chanpref_get(const chanpref *p, const m3u_channel *c)
{
  char *k = pref_key(c);
  long i = k ? pref_find(p, k) : -1;
  free(k);
  return i >= 0 ? p->zoom[i] : -1;
}

int chanpref_set(chanpref *p, const m3u_channel *c, int zoom)
{
  char *k = pref_key(c);
  return k ? pref_put(p, k, zoom) : -1;
}

char *chanpref_format(const chanpref *p)
{
  size_t n = 1;
  for (size_t i = 0; i < p->n; i++) n += strlen(p->key[i]) + 4;
  char *out = malloc(n), *o = out;
  if (!out) return NULL;
  for (size_t i = 0; i < p->n; i++) o += sprintf(o, "%s\t%d\n", p->key[i], p->zoom[i]);
  *o = '\0';
  return out;
}

void chanpref_free(chanpref *p)
{
  for (size_t i = 0; i < p->n; i++) free(p->key[i]);
  free(p->key);
  free(p->zoom);
  memset(p, 0, sizeof(*p));
}
