/* remote.h - remote control over HTTP: tune a channel from Zaparoo, a script
 * or a phone (GET /tune?ch=36). A tiny one-connection-at-a-time server on its
 * own thread; the core applies the commands in retro_run. */
#ifndef RETROTV_REMOTE_H
#define RETROTV_REMOTE_H

#include <stddef.h>

#include "m3u.h"

#define REMOTE_PORT 7474

typedef enum { RC_NONE, RC_PAGE, RC_HELP, RC_STATUS, RC_CHANNELS, RC_SETTINGS, RC_SET, RC_FOCUS, RC_TUNE, RC_UP, RC_DOWN, RC_LAST, RC_BAD } remote_kind;

typedef struct {
  remote_kind kind;
  char ch[32];         /* /tune?ch=36 (decoded) */
  char name[96];       /* /tune?name=next%20generation (decoded) */
  /* /set?sleep=30&audio=2&leveller=loud&picture=zoom&output=240: -1 = not given */
  int sleep;           /* minutes, 0 = off (the core checks it's one of its steps) */
  int audio;           /* track, 1-based */
  int leveller;        /* 0 off, 1 normal, 2 loud */
  int picture;         /* 0 fit, 1 zoom */
  int output;          /* 0 = 640x480, 1 = 240p */
  int screen2;         /* /set?screen2=on|off (dual mode): 1 on, 0 off */
  int screen;          /* screen=1|2 on /tune /up /down /last /focus /settings /set; 0 = the focused one */
} remote_cmd;

/* Parse an HTTP request's first line ("GET /tune?ch=36 HTTP/1.1").
 * Unknown paths give RC_NONE (404), other methods, a bad /tune or a /set
 * with no valid setting (or any invalid one) RC_BAD (400). */
void remote_parse(const char *request, remote_cmd *c);

/* Channel index for /tune: ch matches the channel number exactly; name
 * matches the channel name ignoring case, spaces and punctuation, exactly
 * first, else as a part. -1 if nothing matches. */
int remote_find(const m3u_list *l, const char *ch, const char *name);

/* A reply body that grows as it's written (up to REMOTE_MAX_BODY; past that,
 * or out of memory, it's marked failed and the server answers 500). */
#define REMOTE_MAX_BODY (1 << 20)
typedef struct {
  char *p;
  size_t len, cap;
  int failed;
  const char *type;    /* Content-Type; NULL: JSON if the body starts with '{', else text */
} remote_buf;

void remote_puts(remote_buf *b, const char *s);
void remote_printf(remote_buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* s as a JSON string, quotes included (NULL gives null) */
void remote_json(remote_buf *b, const char *s);
void remote_buf_free(remote_buf *b);

/* Answer one request, on the server thread: write the body into out and
 * return the HTTP status (200, 400, 404, 503). */
typedef int (*remote_handler)(const remote_cmd *c, remote_buf *out, void *ctx);

/* Listen on 127.0.0.1 (lan = 0) or every interface (lan = 1). port 0 picks
 * a free one (tests). Returns 0, or an errno (port taken, ...). */
int remote_start(int lan, int port, remote_handler fn, void *ctx);
void remote_stop(void);
int remote_port(void);   /* the bound port, 0 when stopped */

#endif
