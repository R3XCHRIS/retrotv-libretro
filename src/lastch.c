#include "lastch.h"

#include <stdio.h>
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
