#include "gunzip.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The few zlib declarations we need. z_stream's layout has been stable
 * since zlib 1.0; inflateInit2_ checks the version and the struct size. */
typedef struct {
  const unsigned char *next_in;
  unsigned avail_in;
  unsigned long total_in;
  unsigned char *next_out;
  unsigned avail_out;
  unsigned long total_out;
  const char *msg;
  void *state;
  void *zalloc, *zfree, *opaque;
  int data_type;
  unsigned long adler, reserved;
} zs;

#define Z_OK 0
#define Z_STREAM_END 1
#define Z_BUF_ERROR (-5)
#define GZIP_WBITS (15 + 16)       /* MAX_WBITS + 16: expect a gzip header */

static int (*z_init)(zs *, int, const char *, int);
static int (*z_inflate)(zs *, int);
static int (*z_end)(zs *);
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void load(void)
{
  static const char *names[] = { "libz.so.1", "libz.so", "libz.1.dylib", "libz.dylib" };
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!h) continue;
    z_init = (int (*)(zs *, int, const char *, int))dlsym(h, "inflateInit2_");
    z_inflate = (int (*)(zs *, int))dlsym(h, "inflate");
    z_end = (int (*)(zs *))dlsym(h, "inflateEnd");
    if (z_init && z_inflate && z_end) return;
    z_init = NULL;
  }
}

int gunzip_is_gzip(const void *data, size_t len)
{
  const unsigned char *b = data;
  return len >= 2 && b[0] == 0x1f && b[1] == 0x8b;
}

char *gunzip(const void *data, size_t len, size_t max, size_t *out_len, char *err, size_t errlen)
{
  pthread_once(&once, load);
  if (!z_init) { snprintf(err, errlen, "zlib not found (needed for .gz guides)"); return NULL; }
  size_t cap = len * 8 + 4096 < max ? len * 8 + 4096 : max;
  char *out = malloc(cap + 1);
  if (!out) { snprintf(err, errlen, "out of memory"); return NULL; }
  zs s;
  memset(&s, 0, sizeof(s));
  s.next_in = data;
  s.avail_in = (unsigned)len;
  if (z_init(&s, GZIP_WBITS, "1.2.11", (int)sizeof(s)) != Z_OK) {
    free(out);
    snprintf(err, errlen, "zlib init failed");
    return NULL;
  }
  int rc;
  for (;;) {
    if (s.total_out == cap) {                          /* output full: grow, up to max */
      if (cap >= max) { rc = -100; break; }
      size_t nc = cap * 2 < max ? cap * 2 : max;
      char *no = realloc(out, nc + 1);
      if (!no) { rc = -101; break; }
      out = no;
      cap = nc;
    }
    s.next_out = (unsigned char *)out + s.total_out;
    s.avail_out = (unsigned)(cap - s.total_out);
    rc = z_inflate(&s, 0);
    if (rc == Z_STREAM_END) break;
    if (rc == Z_OK) continue;                          /* progress; go on */
    if (rc == Z_BUF_ERROR && s.avail_out == 0) continue;
    if (rc == Z_BUF_ERROR) rc = -102;                  /* input ended before the stream did */
    break;
  }
  size_t n = s.total_out;
  z_end(&s);
  if (rc != Z_STREAM_END) {
    free(out);
    snprintf(err, errlen, rc == -100 ? "guide too big after unpacking" : rc == -102 ? "gzip data is truncated" : "gzip data is corrupt");
    return NULL;
  }
  out[n] = '\0';
  if (out_len) *out_len = n;
  return out;
}
