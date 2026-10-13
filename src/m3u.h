/* m3u.h - IPTV-style M3U playlist parser (#EXTM3U / #EXTINF with tvg-* attributes). */
#ifndef RETROTV_M3U_H
#define RETROTV_M3U_H

#include <stddef.h>

typedef struct {
  char *name;      /* display name: text after the first unquoted comma in #EXTINF */
  char *url;       /* stream URL, or absolute path for local entries */
  char *number;    /* tvg-chno if present, else 1-based position */
  char *tvg_id;
  char *tvg_name;
  char *logo;      /* tvg-logo */
  char *group;     /* group-title */
} m3u_channel;

typedef struct {
  m3u_channel *ch;
  size_t count;
  char *epg_url;   /* x-tvg-url / url-tvg from the #EXTM3U header */
} m3u_list;

/* Parse playlist text. base_dir resolves relative entries (may be NULL).
 * Returns 0 on success (even if empty), -1 on allocation failure. */
int m3u_parse(const char *text, const char *base_dir, m3u_list *out);
/* Read and parse a file; base_dir is the file's directory. */
int m3u_load(const char *path, m3u_list *out);
void m3u_free(m3u_list *list);

/* True if s looks like scheme://... (http, https, udp, ...). */
int m3u_is_url(const char *s);

/* Copy of s with non-ASCII bytes (emoji etc.) removed and whitespace trimmed. */
char *m3u_ascii(const char *s);

#endif
