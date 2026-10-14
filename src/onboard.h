/* onboard.h - help for first-time setup: turn playlist failures into a plain
 * problem line and a hint the user can act on. */
#ifndef RETROTV_ONBOARD_H
#define RETROTV_ONBOARD_H

#include <stddef.h>

typedef enum {
  OB_NO_FILE,        /* no playlist.txt on any storage */
  OB_EMPTY_FILE,     /* playlist.txt has no link yet (only comments/blank) */
  OB_FETCH,          /* downloading or opening the playlist failed: err holds FFmpeg's text */
  OB_NOT_M3U,        /* got text, but it isn't a playlist */
  OB_NO_CHANNELS,    /* a playlist with no channels */
} ob_kind;

/* Problem (short, shown large) and hint (one or two lines) for a failure. */
void onboard_explain(ob_kind kind, const char *err, char *problem, size_t pn, char *hint, size_t hn);

/* True if text looks like an M3U playlist (#EXTM3U, #EXTINF or a URL line). */
int onboard_is_m3u(const char *text);

#endif
