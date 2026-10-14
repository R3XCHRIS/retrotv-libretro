/* plconf.h - playlist.txt: the playlist link (first line that isn't blank or
 * a # comment) and optional settings lines:
 *   epg=<URL or path>   programme guide to use instead of the playlist's x-tvg-url */
#ifndef RETROTV_PLCONF_H
#define RETROTV_PLCONF_H

#include <stddef.h>

typedef struct {
  char playlist[1024];     /* "" if none */
  char epg[1024];          /* "" if none */
} plconf;

/* Parse text; relative paths are resolved against dir (may be NULL). */
void plconf_parse(const char *text, const char *dir, plconf *out);

#endif
