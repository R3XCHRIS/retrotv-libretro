/* Host unit tests for the remote control (parser, channel lookup, a real
 * request over loopback). Build: make test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "../src/remote.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static int calls;
static int fake(const remote_cmd *c, char *body, size_t n, void *ctx)
{
  calls++;
  const m3u_list *l = ctx;
  if (c->kind != RC_TUNE) { snprintf(body, n, "kind %d\n", c->kind); return c->kind == RC_NONE ? 404 : 200; }
  int i = remote_find(l, c->ch, c->name);
  if (i < 0) { snprintf(body, n, "{\"ok\":false}"); return 404; }
  char nm[128];
  remote_json_str(nm, sizeof(nm), l->ch[i].name);
  snprintf(body, n, "{\"ok\":true,\"name\":%s}", nm);
  return 200;
}

/* one request to the server; the whole response back */
static void get(int port, const char *req, char *out, size_t n)
{
  out[0] = '\0';
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = { 0 };
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&a, sizeof(a))) { close(fd); return; }
  send(fd, req, strlen(req), 0);
  size_t got = 0;
  ssize_t r;
  while (got + 1 < n && (r = recv(fd, out + got, n - 1 - got, 0)) > 0) got += (size_t)r;
  out[got] = '\0';
  close(fd);
}

int main(void)
{
  remote_cmd c;
  remote_parse("GET /tune?ch=36 HTTP/1.1\r\nHost: x\r\n\r\n", &c);
  CHECK(c.kind == RC_TUNE && !strcmp(c.ch, "36") && !*c.name);
  remote_parse("GET /tune?name=next%20generation&x=1 HTTP/1.1\r\n", &c);
  CHECK(c.kind == RC_TUNE && !strcmp(c.name, "next generation") && !*c.ch);
  remote_parse("GET /tune?name=star+trek HTTP/1.0\r\n", &c);
  CHECK(c.kind == RC_TUNE && !strcmp(c.name, "star trek"));
  remote_parse("GET /tune?name=50%25%zz HTTP/1.1\r\n", &c);       /* bad escapes kept as text */
  CHECK(c.kind == RC_TUNE && !strcmp(c.name, "50%%zz"));
  remote_parse("GET /tune HTTP/1.1\r\n", &c);
  CHECK(c.kind == RC_BAD);
  remote_parse("GET /tune?ch= HTTP/1.1\r\n", &c);
  CHECK(c.kind == RC_BAD);
  remote_parse("POST /up HTTP/1.1\r\n", &c);
  CHECK(c.kind == RC_UP);
  remote_parse("GET /down/ HTTP/1.1\r\n", &c);
  CHECK(c.kind == RC_DOWN);
  remote_parse("GET /last HTTP/1.1\r\n", &c);
  CHECK(c.kind == RC_LAST);
  remote_parse("GET /status HTTP/1.1\r\n", &c);
  CHECK(c.kind == RC_STATUS);
  remote_parse("GET / HTTP/1.1\r\n", &c);
  CHECK(c.kind == RC_HELP);
  remote_parse("GET /tuner HTTP/1.1\r\n", &c);
  CHECK(c.kind == RC_NONE);
  remote_parse("DELETE /up HTTP/1.1\r\n", &c);
  CHECK(c.kind == RC_BAD);
  remote_parse("garbage", &c);
  CHECK(c.kind == RC_BAD);
  remote_parse("", &c);
  CHECK(c.kind == RC_BAD);

  m3u_list l = { 0 };
  CHECK(!m3u_parse("#EXTM3U\n"
                   "#EXTINF:-1 tvg-chno=\"37\",Star Trek The Original Series\nhttp://x/1\n"
                   "#EXTINF:-1 tvg-chno=\"36\",Star Trek The Next Generation\nhttp://x/2\n"
                   "#EXTINF:-1 tvg-chno=\"5A\",Monkey\nhttp://x/3\n"
                   "#EXTINF:-1,M*A*S*H\nhttp://x/4\n", NULL, &l));
  CHECK(remote_find(&l, "36", NULL) == 1);
  CHECK(remote_find(&l, " 37 ", NULL) == 0);
  CHECK(remote_find(&l, "5a", NULL) == 2);
  CHECK(remote_find(&l, "4", NULL) == 3);                  /* no tvg-chno: position */
  CHECK(remote_find(&l, "99", NULL) == -1);
  CHECK(remote_find(&l, "99", "monkey") == -1);            /* ch wins over name */
  CHECK(remote_find(&l, NULL, "next generation") == 1);
  CHECK(remote_find(&l, NULL, "STAR TREK") == 0);          /* first part match */
  CHECK(remote_find(&l, NULL, "monkey") == 2);
  CHECK(remote_find(&l, NULL, "mash") == 3);               /* punctuation ignored */
  CHECK(remote_find(&l, NULL, "!!") == -1);
  CHECK(remote_find(&l, NULL, "") == -1);
  CHECK(remote_find(NULL, "1", NULL) == -1);

  char j[32];
  remote_json_str(j, sizeof(j), "a\"b\\c\n");
  CHECK(!strcmp(j, "\"a\\\"b\\\\c\\u000a\""));
  remote_json_str(j, 8, "abcdefghij");
  CHECK(!strcmp(j, "\"abcde\""));
  remote_json_str(j, sizeof(j), NULL);
  CHECK(!strcmp(j, "\"\""));

  /* the real server over loopback */
  CHECK(remote_start(0, 0, fake, &l) == 0);
  int port = remote_port();
  CHECK(port > 0);
  CHECK(remote_start(0, 0, fake, &l) != 0);                /* only one */
  char r[2048];
  get(port, "GET /tune?ch=36 HTTP/1.1\r\nHost: pi\r\n\r\n", r, sizeof(r));
  CHECK(!strncmp(r, "HTTP/1.1 200 OK\r\n", 17));
  CHECK(strstr(r, "Content-Type: application/json") != NULL);
  CHECK(strstr(r, "\r\n\r\n{\"ok\":true,\"name\":\"Star Trek The Next Generation\"}") != NULL);
  get(port, "GET /tune?ch=99 HTTP/1.1\r\n\r\n", r, sizeof(r));
  CHECK(!strncmp(r, "HTTP/1.1 404 ", 13));
  get(port, "GET /nope HTTP/1.1\r\n\r\n", r, sizeof(r));
  CHECK(!strncmp(r, "HTTP/1.1 404 ", 13) && strstr(r, "text/plain"));
  get(port, "GET /up HTTP/1.0\n\n", r, sizeof(r));          /* bare newlines */
  CHECK(!strncmp(r, "HTTP/1.1 200 ", 13));
  int before = calls;
  int fd = socket(AF_INET, SOCK_STREAM, 0);                 /* a client that sends nothing */
  struct sockaddr_in a = { 0 };
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(!connect(fd, (struct sockaddr *)&a, sizeof(a)));
  close(fd);
  get(port, "GET /status HTTP/1.1\r\n\r\n", r, sizeof(r));   /* still serving */
  CHECK(!strncmp(r, "HTTP/1.1 200 ", 13) && calls == before + 1);
  remote_stop();
  CHECK(remote_port() == 0);
  get(port, "GET /up HTTP/1.1\r\n\r\n", r, sizeof(r));
  CHECK(r[0] == '\0');                                     /* closed */
  remote_stop();                                           /* twice is fine */
  CHECK(remote_start(0, 0, fake, &l) == 0);                /* and again */
  remote_stop();

  m3u_free(&l);
  if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
  printf("test_remote: all passed\n");
  return 0;
}
