/* Host tests for zapnow: the JSON body, and the sender against a stand-in
 * Zaparoo on a local port. Build: make test */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "../src/zapnow.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

/* a stand-in Zaparoo: accept, keep the request, answer 200 */
static int lfd, got_n;
static char got[2048];
static void *fake(void *arg)
{
  (void)arg;
  for (;;) {
    int c = accept(lfd, NULL, NULL);
    if (c < 0) return NULL;
    char buf[2048];
    ssize_t r = recv(c, buf, sizeof(buf) - 1, 0);
    if (r > 0) { buf[r] = 0; memcpy(got, buf, (size_t)r + 1); got_n++; }
    const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";
    send(c, ok, strlen(ok), MSG_NOSIGNAL);
    close(c);
  }
}

static void wait_for(int n)
{
  for (int i = 0; i < 200 && got_n < n; i++) usleep(10000);
}

int main(void)
{
  char b[700];
  CHECK(zapnow_body(b, sizeof(b), "RetroTV: 36 Star Trek", "36") > 0);
  CHECK(!strcmp(b, "{\"jsonrpc\":\"2.0\",\"id\":\"retrotv\",\"method\":\"media.active.update\","
                   "\"params\":{\"systemId\":\"Video\",\"mediaName\":\"RetroTV: 36 Star Trek\",\"mediaPath\":\"retrotv://36\"}}"));
  CHECK(zapnow_body(b, sizeof(b), "Say \"Hi\" \\ now\n", "1") > 0);           /* escaped */
  CHECK(strstr(b, "\"mediaName\":\"Say \\\"Hi\\\" \\\\ now\\u000a\"") != NULL);
  CHECK(zapnow_body(b, 40, "RetroTV: 36 Star Trek", "36") == -1);          /* doesn't fit */

  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0 };
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t al = sizeof(a);
  lfd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(lfd >= 0 && !bind(lfd, (struct sockaddr *)&a, sizeof(a)) && !listen(lfd, 4));
  getsockname(lfd, (struct sockaddr *)&a, &al);
  int port = ntohs(a.sin_port);
  pthread_t t;
  pthread_create(&t, NULL, fake, NULL);

  char note[160];
  zapnow_report("RetroTV: 36 Star Trek", "36", port);
  wait_for(1);
  CHECK(got_n == 1);
  CHECK(!strncmp(got, "POST /api HTTP/1.1\r\n", 20) && strstr(got, "\"mediaName\":\"RetroTV: 36 Star Trek\""));
  usleep(50000);
  zapnow_take_note(note, sizeof(note));
  CHECK(!strcmp(note, "reporting what's on"));
  zapnow_report("RetroTV: 36 Star Trek", "36", port);                 /* unchanged: not sent again */
  usleep(150000);
  CHECK(got_n == 1);
  zapnow_report("RetroTV: 7 CNN", "7", port);
  wait_for(2);
  CHECK(got_n == 2 && strstr(got, "retrotv://7"));
  zapnow_take_note(note, sizeof(note));
  CHECK(!*note);                                                      /* same outcome: nothing new to log */

  shutdown(lfd, SHUT_RDWR);                                           /* Zaparoo goes away */
  close(lfd);
  pthread_join(t, NULL);
  zapnow_report("RetroTV: 9 News", "9", port);
  for (int i = 0; i < 150 && !*note; i++) { usleep(10000); zapnow_take_note(note, sizeof(note)); }
  CHECK(strstr(note, "not answering") != NULL);
  zapnow_stop();
  zapnow_stop();                                                      /* twice is fine */

  if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
  puts("test_zapnow: all passed");
  return 0;
}
