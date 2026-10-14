/* remote.h - remote control over HTTP: tune a channel from Zaparoo, a script
 * or a phone (GET /tune?ch=36). A tiny one-connection-at-a-time server on its
 * own thread; the core applies the commands in retro_run. */
#ifndef RETROTV_REMOTE_H
#define RETROTV_REMOTE_H

#include <stddef.h>

#include "m3u.h"

#define REMOTE_PORT 7474

typedef enum { RC_NONE, RC_HELP, RC_STATUS, RC_TUNE, RC_UP, RC_DOWN, RC_LAST, RC_BAD } remote_kind;

typedef struct {
  remote_kind kind;
  char ch[32];         /* /tune?ch=36 (decoded) */
  char name[96];       /* /tune?name=next%20generation (decoded) */
} remote_cmd;

/* Parse an HTTP request's first line ("GET /tune?ch=36 HTTP/1.1").
 * Unknown paths give RC_NONE (404), other methods or a bad /tune RC_BAD (400). */
void remote_parse(const char *request, remote_cmd *c);

/* Channel index for /tune: ch matches the channel number exactly; name
 * matches the channel name ignoring case, spaces and punctuation, exactly
 * first, else as a part. -1 if nothing matches. */
int remote_find(const m3u_list *l, const char *ch, const char *name);

/* s as a JSON string, quotes included; cut to fit n. */
void remote_json_str(char *out, size_t n, const char *s);

/* Answer one request, on the server thread: write the JSON (or text) body
 * into body and return the HTTP status (200, 400, 404, 503). */
typedef int (*remote_handler)(const remote_cmd *c, char *body, size_t n, void *ctx);

/* Listen on 127.0.0.1 (lan = 0) or every interface (lan = 1). port 0 picks
 * a free one (tests). Returns 0, or an errno (port taken, ...). */
int remote_start(int lan, int port, remote_handler fn, void *ctx);
void remote_stop(void);
int remote_port(void);   /* the bound port, 0 when stopped */

#endif
