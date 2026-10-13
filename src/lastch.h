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

#endif
