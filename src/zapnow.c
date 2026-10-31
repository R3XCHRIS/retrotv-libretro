#include "zapnow.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

/* s as JSON string content (no quotes); returns chars written, -1 if it doesn't fit */
static int json_str(char *out, size_t n, const char *s)
{
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    char esc[8];
    const char *w = esc;
    if (*p == '"' || *p == '\\') { esc[0] = '\\'; esc[1] = (char)*p; esc[2] = 0; }
    else if (*p < 0x20) snprintf(esc, sizeof(esc), "\\u%04x", *p);
    else { esc[0] = (char)*p; esc[1] = 0; }
    size_t l = strlen(w);
    if (o + l >= n) return -1;
    memcpy(out + o, w, l);
    o += l;
  }
  out[o] = 0;
  return (int)o;
}

int zapnow_body(char *out, size_t n, const char *name, const char *path)
{
  char jn[512], jp[160];
  if (json_str(jn, sizeof(jn), name) < 0 || json_str(jp, sizeof(jp), path) < 0) return -1;
  int r = snprintf(out, n,
                   "{\"jsonrpc\":\"2.0\",\"id\":\"retrotv\",\"method\":\"media.active.update\","
                   "\"params\":{\"systemId\":\"Video\",\"mediaName\":\"%s\",\"mediaPath\":\"retrotv://%s\"}}",
                   jn, jp);
  return r < 0 || (size_t)r >= n ? -1 : r;
}

/* ---------- sender thread: newest report wins ---------- */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static int live, quit, port_;
static char want[600], sent[600];      /* body to send / last body sent */
static char note[160], last_note[160];

static void set_note(const char *s)
{
  pthread_mutex_lock(&mu);
  if (strcmp(s, last_note)) {          /* only changes reach the log */
    snprintf(note, sizeof(note), "%s", s);
    snprintf(last_note, sizeof(last_note), "%s", s);
  }
  pthread_mutex_unlock(&mu);
}

/* one POST to 127.0.0.1:port, 1 s at most; 0 when Zaparoo answered 200 */
static int post(const char *body, int port, char *why, size_t wn)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { snprintf(why, wn, "socket: %s", strerror(errno)); return -1; }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((unsigned short)port) };
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int rc = connect(fd, (struct sockaddr *)&a, sizeof(a));
  struct pollfd pf = { fd, POLLOUT, 0 };
  if (rc < 0 && errno == EINPROGRESS) {
    if (poll(&pf, 1, 500) == 1) {
      int err = 0;
      socklen_t el = sizeof(err);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
      rc = err ? -1 : 0;
      errno = err;
    } else errno = ETIMEDOUT;
  }
  if (rc < 0) { snprintf(why, wn, "not answering on port %d (%s)", port, strerror(errno ? errno : ETIMEDOUT)); close(fd); return -1; }
  char req[900];
  int l = snprintf(req, sizeof(req),
                   "POST /api HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Type: application/json\r\n"
                   "Content-Length: %zu\r\nConnection: close\r\n\r\n%s", port, strlen(body), body);
  int ok = l > 0 && (size_t)l < sizeof(req) && send(fd, req, (size_t)l, MSG_NOSIGNAL) == l;
  char resp[64] = "";
  pf.events = POLLIN;
  if (ok && poll(&pf, 1, 1000) == 1) {
    ssize_t r = recv(fd, resp, sizeof(resp) - 1, 0);
    if (r > 0) resp[r] = 0;
  }
  close(fd);
  if (!ok || strncmp(resp, "HTTP/1.1 200", 12)) {
    snprintf(why, wn, "unexpected reply on port %d: %.20s", port, *resp ? resp : "(none)");
    return -1;
  }
  return 0;
}

static void *sender(void *arg)
{
  (void)arg;
  pthread_mutex_lock(&mu);
  while (!quit) {
    if (!*want || !strcmp(want, sent)) { pthread_cond_wait(&cv, &mu); continue; }
    char body[sizeof(want)];
    snprintf(body, sizeof(body), "%s", want);
    int port = port_;
    pthread_mutex_unlock(&mu);
    char why[120] = "";
    int rc = post(body, port, why, sizeof(why));
    set_note(rc ? why : "reporting what's on");
    pthread_mutex_lock(&mu);
    snprintf(sent, sizeof(sent), "%s", body);    /* a failed send isn't retried until the channel changes */
  }
  pthread_mutex_unlock(&mu);
  return NULL;
}

void zapnow_report(const char *name, const char *path, int port)
{
  char body[sizeof(want)];
  if (zapnow_body(body, sizeof(body), name, path) < 0) return;
  pthread_mutex_lock(&mu);
  if (!live) {
    quit = 0;
    sent[0] = 0;
    live = !pthread_create(&thread, NULL, sender, NULL);
  }
  port_ = port > 0 ? port : ZAPNOW_PORT;
  snprintf(want, sizeof(want), "%s", body);
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mu);
}

void zapnow_take_note(char *out, size_t n)
{
  pthread_mutex_lock(&mu);
  snprintf(out, n, "%s", note);
  note[0] = 0;
  pthread_mutex_unlock(&mu);
}

void zapnow_stop(void)
{
  pthread_mutex_lock(&mu);
  int was = live;
  quit = 1;
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mu);
  if (was) pthread_join(thread, NULL);
  pthread_mutex_lock(&mu);
  live = 0;
  want[0] = sent[0] = note[0] = last_note[0] = 0;
  pthread_mutex_unlock(&mu);
}
