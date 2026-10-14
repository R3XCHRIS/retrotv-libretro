#include "remote.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0           /* macOS (host tests): SO_NOSIGPIPE below instead */
#endif

/* ---------- parsing ---------- */

static int hexval(int c)
{
  if (c >= '0' && c <= '9') return c - '0';
  c = tolower(c);
  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* URL-decode src[0..len) into out (%XX and '+'), cut to fit n */
static void url_decode(char *out, size_t n, const char *src, size_t len)
{
  size_t o = 0;
  for (size_t i = 0; i < len && o + 1 < n; i++) {
    int c = (unsigned char)src[i];
    if (c == '+') c = ' ';
    else if (c == '%' && i + 2 < len && hexval(src[i + 1]) >= 0 && hexval(src[i + 2]) >= 0) {
      c = hexval(src[i + 1]) * 16 + hexval(src[i + 2]);
      i += 2;
    }
    if (c) out[o++] = (char)c;
  }
  out[o] = '\0';
}

/* value of key in a query string "a=1&b=2" (decoded); 1 if present */
static int query_get(const char *q, size_t qlen, const char *key, char *out, size_t n)
{
  size_t kl = strlen(key);
  const char *p = q, *end = q + qlen;
  while (p < end) {
    const char *amp = memchr(p, '&', (size_t)(end - p));
    const char *e = amp ? amp : end;
    if ((size_t)(e - p) > kl && !strncmp(p, key, kl) && p[kl] == '=') {
      url_decode(out, n, p + kl + 1, (size_t)(e - p - (ptrdiff_t)kl - 1));
      return 1;
    }
    p = e + 1;
  }
  return 0;
}

void remote_parse(const char *req, remote_cmd *c)
{
  memset(c, 0, sizeof(*c));
  const char *sp = strchr(req, ' ');
  if (!sp) { c->kind = RC_BAD; return; }
  size_t ml = (size_t)(sp - req);
  if (!((ml == 3 && !strncmp(req, "GET", 3)) || (ml == 4 && !strncmp(req, "POST", 4)))) { c->kind = RC_BAD; return; }
  const char *path = sp + 1;
  size_t pl = strcspn(path, " \r\n");
  const char *qm = memchr(path, '?', pl);
  size_t plen = qm ? (size_t)(qm - path) : pl;
  const char *q = qm ? qm + 1 : path + pl;
  size_t qlen = qm ? pl - plen - 1 : 0;
  while (plen > 1 && path[plen - 1] == '/') plen--;           /* "/up/" = "/up" */
#define IS(s) (plen == strlen(s) && !strncmp(path, s, plen))
  if (IS("/")) c->kind = RC_PAGE;
  else if (IS("/help")) c->kind = RC_HELP;
  else if (IS("/status")) c->kind = RC_STATUS;
  else if (IS("/channels")) c->kind = RC_CHANNELS;
  else if (IS("/up")) c->kind = RC_UP;
  else if (IS("/down")) c->kind = RC_DOWN;
  else if (IS("/last")) c->kind = RC_LAST;
  else if (IS("/tune")) {
    int a = query_get(q, qlen, "ch", c->ch, sizeof(c->ch));
    int b = query_get(q, qlen, "name", c->name, sizeof(c->name));
    c->kind = (a && *c->ch) || (b && *c->name) ? RC_TUNE : RC_BAD;
  } else c->kind = RC_NONE;
#undef IS
}

/* lowercase letters and digits only: "Star Trek: TNG" -> "startrektng" */
static void norm(char *out, size_t n, const char *s)
{
  size_t o = 0;
  for (; s && *s && o + 1 < n; s++)
    if (isalnum((unsigned char)*s)) out[o++] = (char)tolower((unsigned char)*s);
  out[o] = '\0';
}

static void trim_copy(char *out, size_t n, const char *s)
{
  while (*s == ' ' || *s == '\t') s++;
  snprintf(out, n, "%s", s);
  size_t l = strlen(out);
  while (l && (out[l - 1] == ' ' || out[l - 1] == '\t')) out[--l] = '\0';
}

int remote_find(const m3u_list *l, const char *ch, const char *name)
{
  if (!l) return -1;
  if (ch && *ch) {
    char want[32];
    trim_copy(want, sizeof(want), ch);
    for (size_t i = 0; i < l->count; i++)
      if (l->ch[i].number && !strcasecmp(l->ch[i].number, want)) return (int)i;
    return -1;
  }
  char want[96], have[256];
  norm(want, sizeof(want), name);
  if (!*want) return -1;
  for (size_t i = 0; i < l->count; i++) {
    norm(have, sizeof(have), l->ch[i].name);
    if (!strcmp(have, want)) return (int)i;
  }
  for (size_t i = 0; i < l->count; i++) {
    norm(have, sizeof(have), l->ch[i].name);
    if (strstr(have, want)) return (int)i;
  }
  return -1;
}

/* ---------- reply body ---------- */

static int grow(remote_buf *b, size_t more)
{
  if (b->failed) return -1;
  if (b->len + more + 1 <= b->cap) return 0;
  size_t cap = b->cap ? b->cap : 1024;
  while (cap < b->len + more + 1) cap *= 2;
  if (cap > REMOTE_MAX_BODY) { b->failed = 1; return -1; }
  char *p = realloc(b->p, cap);
  if (!p) { b->failed = 1; return -1; }
  b->p = p;
  b->cap = cap;
  return 0;
}

static void put(remote_buf *b, const char *s, size_t n)
{
  if (grow(b, n)) return;
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = '\0';
}

void remote_puts(remote_buf *b, const char *s) { put(b, s, strlen(s)); }

void remote_printf(remote_buf *b, const char *fmt, ...)
{
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n < 0) { b->failed = 1; return; }
  if ((size_t)n < sizeof(tmp)) { put(b, tmp, (size_t)n); return; }
  if (grow(b, (size_t)n)) return;                     /* long: format straight into the buffer */
  va_start(ap, fmt);
  vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
}

void remote_json(remote_buf *b, const char *s)
{
  if (!s) { put(b, "null", 4); return; }
  put(b, "\"", 1);
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    char esc[8];
    if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; put(b, esc, 2); }
    else if (c < 0x20) { snprintf(esc, sizeof(esc), "\\u%04x", c); put(b, esc, 6); }
    else put(b, (const char *)&c, 1);
  }
  put(b, "\"", 1);
}

void remote_buf_free(remote_buf *b)
{
  free(b->p);
  memset(b, 0, sizeof(*b));
}

/* ---------- server ---------- */

static int lfd = -1;
static pthread_t thr;
static int thr_live;
static atomic_int stop_flag;
static int bound_port;
static remote_handler handler;
static void *handler_ctx;

static const char *reason(int code)
{
  switch (code) {
  case 200: return "OK";
  case 400: return "Bad Request";
  case 404: return "Not Found";
  case 500: return "Internal Server Error";
  default: return "Service Unavailable";
  }
}

static void send_all(int fd, const char *p, size_t n)
{
  while (n) {
    ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
    if (w <= 0) { if (w < 0 && errno == EINTR) continue; return; }
    p += w;
    n -= (size_t)w;
  }
}

static void serve(int fd)
{
  struct timeval tv = { 1, 0 };                       /* a slow client can't hold the server */
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  char req[2048];
  size_t got = 0;
  while (got + 1 < sizeof(req)) {
    ssize_t r = recv(fd, req + got, sizeof(req) - 1 - got, 0);
    if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
    got += (size_t)r;
    req[got] = '\0';
    if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
  }
  req[got] = '\0';
  if (!got) return;
  remote_cmd c;
  remote_parse(req, &c);
  remote_buf body = { 0 };
  int code = handler ? handler(&c, &body, handler_ctx) : 503;
  if (body.failed) {
    free(body.p);
    memset(&body, 0, sizeof(body));
    code = 500;
    remote_puts(&body, "{\"ok\":false,\"error\":\"reply too large\"}");
  }
  const char *bp = body.p ? body.p : "";
  const char *type = body.type ? body.type : bp[0] == '{' ? "application/json" : "text/plain; charset=utf-8";
  char head[320];
  int hl = snprintf(head, sizeof(head),
                    "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                    "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                    code, reason(code), type, body.len);
  send_all(fd, head, (size_t)hl);
  send_all(fd, bp, body.len);
  remote_buf_free(&body);
}

static void *server_main(void *arg)
{
  (void)arg;
  while (!atomic_load(&stop_flag)) {
    struct pollfd p = { lfd, POLLIN, 0 };
    if (poll(&p, 1, 200) <= 0) continue;              /* wake up to check stop_flag */
    int fd = accept(lfd, NULL, NULL);
    if (fd < 0) continue;
    serve(fd);
    close(fd);
  }
  return NULL;
}

int remote_start(int lan, int port, remote_handler fn, void *ctx)
{
  if (thr_live) return EBUSY;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return errno;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
  if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 4)) {
    int e = errno;
    close(fd);
    return e;
  }
  socklen_t al = sizeof(a);
  getsockname(fd, (struct sockaddr *)&a, &al);
  lfd = fd;
  bound_port = ntohs(a.sin_port);
  handler = fn;
  handler_ctx = ctx;
  atomic_store(&stop_flag, 0);
  if (pthread_create(&thr, NULL, server_main, NULL)) {
    int e = errno ? errno : EAGAIN;
    close(lfd);
    lfd = -1;
    bound_port = 0;
    return e;
  }
  thr_live = 1;
  return 0;
}

void remote_stop(void)
{
  if (!thr_live) return;
  atomic_store(&stop_flag, 1);
  pthread_join(thr, NULL);                           /* at most one poll interval plus a request */
  thr_live = 0;
  close(lfd);
  lfd = -1;
  bound_port = 0;
}

int remote_port(void) { return bound_port; }
