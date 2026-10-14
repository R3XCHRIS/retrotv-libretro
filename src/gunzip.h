/* gunzip.h - unpack gzip data (XMLTV guides are often .xml.gz) with the
 * system zlib, loaded at runtime like FFmpeg (no build dependency). */
#ifndef RETROTV_GUNZIP_H
#define RETROTV_GUNZIP_H

#include <stddef.h>

/* True if data starts with the gzip signature. */
int gunzip_is_gzip(const void *data, size_t len);

/* Unpack into a malloc'd, NUL-terminated buffer of at most max bytes.
 * Returns NULL and fills err on failure (zlib missing, corrupt, too big). */
char *gunzip(const void *data, size_t len, size_t max, size_t *out_len, char *err, size_t errlen);

#endif
