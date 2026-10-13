/* lastch.h - remember the last channel watched, per playlist.
 * The file holds the channel's number and name (not its position), so the
 * right channel comes back after the playlist is reordered or grows. */
#ifndef RETROTV_LASTCH_H
#define RETROTV_LASTCH_H

#include <stddef.h>
#include <stdint.h>

#include "m3u.h"

/* Stable short id for a playlist source (FNV-1a of the URL or path). */
uint32_t lastch_hash(const char *src);

/* File contents for a channel: "number=...\nname=...\n". */
void lastch_format(const m3u_channel *c, char *out, size_t n);

/* Index of the saved channel in list, or -1. Matches number and name, then
 * name alone, then number alone. */
int lastch_find(const m3u_list *list, const char *text);

/* Per-channel zoom memory: "number<TAB>name<TAB>0|1" lines. */
typedef struct {
  char **key;              /* "number\tname" */
  unsigned char *zoom;
  size_t n;
} chanpref;

void chanpref_parse(chanpref *p, const char *text);
/* 0 or 1 if remembered for c, else -1 */
int chanpref_get(const chanpref *p, const m3u_channel *c);
/* remember zoom for c; 0 on success */
int chanpref_set(chanpref *p, const m3u_channel *c, int zoom);
/* malloc'd file text */
char *chanpref_format(const chanpref *p);
void chanpref_free(chanpref *p);

#endif
