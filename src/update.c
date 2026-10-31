#include "update.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int update_parse_tag(const char *json, char *tag, size_t n)
{
  if (!json || !n) return -1;
  const char *p = strstr(json, "\"tag_name\"");
  if (!p) return -1;
  p += strlen("\"tag_name\"");
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p++ != ':') return -1;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p++ != '"') return -1;
  size_t o = 0;
  for (; *p && *p != '"'; p++) {
    unsigned char c = (unsigned char)*p;
    if (!isalnum(c) && c != '.' && c != '_' && c != '-') return -1;
    if (o + 1 >= n) return -1;
    tag[o++] = (char)c;
  }
  if (*p != '"' || !o) return -1;
  tag[o] = 0;
  return 0;
}

int update_cmp(const char *a, const char *b)
{
  if (*a == 'v' || *a == 'V') a++;
  if (*b == 'v' || *b == 'V') b++;
  while (*a || *b) {
    long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
    if (x != y) return x < y ? -1 : 1;
    while (*a && *a != '.') a++;                       /* "-rc1" and the like: ignored */
    while (*b && *b != '.') b++;
    if (*a == '.') a++;
    if (*b == '.') b++;
  }
  return 0;
}

int update_sums_has(const char *sums, const char *file)
{
  size_t fl = strlen(file);
  for (const char *l = sums; l && *l;) {
    const char *e = strchr(l, '\n');
    size_t len = e ? (size_t)(e - l) : strlen(l);
    while (len && (l[len - 1] == '\r' || l[len - 1] == ' ')) len--;
    if (len > fl + 1 && !memcmp(l + len - fl, file, fl) && (l[len - fl - 1] == ' ' || l[len - fl - 1] == '*'))
      return 1;
    l = e ? e + 1 : NULL;
  }
  return 0;
}
