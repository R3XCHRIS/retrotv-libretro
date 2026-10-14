#include "plconf.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "m3u.h"

/* URL or absolute path as is; a relative path is made relative to dir */
static void resolve(const char *v, const char *dir, char *out, size_t n)
{
  if (m3u_is_url(v) || v[0] == '/' || !dir || !*dir) snprintf(out, n, "%s", v);
  else snprintf(out, n, "%s/%s", dir, v);
}

void plconf_parse(const char *text, const char *dir, plconf *out)
{
  out->playlist[0] = out->epg[0] = '\0';
  for (const char *l = text; l && *l;) {
    const char *e = strpbrk(l, "\r\n");
    size_t len = e ? (size_t)(e - l) : strlen(l);
    char line[1024];
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, l, len);
    line[len] = '\0';
    char *s = line, *t = line + len;
    while (*s == ' ' || *s == '\t') s++;
    while (t > s && (t[-1] == ' ' || t[-1] == '\t')) *--t = '\0';
    if (*s && *s != '#') {
      if (!strncasecmp(s, "epg", 3) && (s[3] == '=' || s[3] == ':' || s[3] == ' ')) {
        char *v = s + 3;
        while (*v == ' ' || *v == '=' || *v == ':') v++;
        if (*v && !*out->epg) resolve(v, dir, out->epg, sizeof(out->epg));
      } else if (!*out->playlist) {
        resolve(s, dir, out->playlist, sizeof(out->playlist));
      }
    }
    if (!e) break;
    l = e + 1;
  }
}
