/* update.h - pieces of RetroTV's own update check: the latest release's tag
 * from GitHub's API, version comparison, and the release's file list. The
 * thread, the downloads and the installer run live in retrotv.c. */
#ifndef RETROTV_UPDATE_H
#define RETROTV_UPDATE_H

#include <stddef.h>

#define UPDATE_REPO "chris-skitch/retrotv-libretro"

/* The "tag_name" of a GitHub release JSON into tag (e.g. "v0.3.2"). Only
 * letters, digits, '.', '_' and '-' are accepted (it becomes part of a path
 * and a URL). 0 on success. */
int update_parse_tag(const char *json, char *tag, size_t n);

/* Compare versions like "v0.3.10" and "0.3.9" (a leading v is ignored,
 * missing parts count as 0): <0, 0 or >0 like strcmp. */
int update_cmp(const char *a, const char *b);

/* Does SHA256SUMS text list file (as "<hash>  file" or "<hash> *file")? */
int update_sums_has(const char *sums, const char *file);

#endif
