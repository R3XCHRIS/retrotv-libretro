/* Host tests for update.c: release tag, version order, SHA256SUMS. Build: make test */
#include <stdio.h>
#include <string.h>

#include "../src/update.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

int main(void)
{
  char t[32];
  CHECK(!update_parse_tag("{\"url\":\"x\",\"tag_name\":\"v0.3.2\",\"name\":\"RetroTV v0.3.2\"}", t, sizeof(t)) && !strcmp(t, "v0.3.2"));
  CHECK(!update_parse_tag("{\n  \"tag_name\" : \"v1.0.0-rc1\"\n}", t, sizeof(t)) && !strcmp(t, "v1.0.0-rc1"));
  CHECK(update_parse_tag("{\"tag_name\":\"v1/../../x\"}", t, sizeof(t)));        /* no path tricks */
  CHECK(update_parse_tag("{\"tag_name\":\"v1 2\"}", t, sizeof(t)));
  CHECK(update_parse_tag("{\"tag_name\":\"\"}", t, sizeof(t)));
  CHECK(update_parse_tag("{\"message\":\"Not Found\"}", t, sizeof(t)));
  CHECK(update_parse_tag("{\"tag_name\":\"v0.3.2\"}", t, 4));                    /* too long for the buffer */
  CHECK(update_parse_tag(NULL, t, sizeof(t)));

  CHECK(update_cmp("v0.3.2", "0.3.1") > 0);
  CHECK(update_cmp("v0.3.10", "0.3.9") > 0);
  CHECK(update_cmp("v0.3.1", "0.3.1") == 0);
  CHECK(update_cmp("v0.3", "0.3.0") == 0);
  CHECK(update_cmp("v0.2.10", "0.3.0") < 0);
  CHECK(update_cmp("v1.0.0", "0.9.99") > 0);
  CHECK(update_cmp("v0.3.2-rc1", "0.3.2") == 0);

  const char *sums = "aaaa  retrotv_libretro.so\nbbbb *install.sh\r\ncccc  retrotv_install.sh\n";
  CHECK(update_sums_has(sums, "retrotv_libretro.so"));
  CHECK(update_sums_has(sums, "install.sh"));
  CHECK(update_sums_has(sums, "retrotv_install.sh"));
  CHECK(!update_sums_has(sums, "retrotv_launch.sh"));
  CHECK(!update_sums_has(sums, "libretro.so"));                                /* whole names only */
  CHECK(!update_sums_has("", "install.sh"));

  if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
  puts("test_update: all passed");
  return 0;
}
