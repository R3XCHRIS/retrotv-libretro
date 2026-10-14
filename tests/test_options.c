/* Host unit tests for the core options table. Build: make test */
#include <stdio.h>
#include <string.h>

#include "../src/options.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static const char *map_get(const char *key, void *ctx)
{
  const char **kv = ctx;
  for (int i = 0; kv[i]; i += 2) if (!strcmp(kv[i], key)) return kv[i + 1];
  return NULL;
}

/* fake frontend */
static unsigned fe_version;
static const struct retro_core_options_v2 *got_v2;
static const struct retro_variable *got_v0;
static bool fake_env(unsigned cmd, void *data)
{
  switch (cmd) {
  case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = fe_version; return true;
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: got_v2 = data; return fe_version >= 2;
  case RETRO_ENVIRONMENT_SET_VARIABLES: got_v0 = data; return true;
  case RETRO_ENVIRONMENT_GET_VARIABLE: {
    struct retro_variable *v = data;
    v->value = !strcmp(v->key, "retrotv_clock") ? "12" : NULL;
    return v->value != NULL;
  }
  default: return false;
  }
}

int main(void)
{
  rtv_options o;
  options_defaults(&o);
  CHECK(o.banner_secs == 4 && o.clock24 && o.hints && o.leveller == 1 && o.static_vol == 2);
  CHECK(o.theme == 0 && !o.static_vhs && !o.blip);
  CHECK(o.overscan == 0 && o.refresh == 0 && !o.zoom_default && o.zoom_remember && !o.res240);

  const char *kv[] = { "retrotv_banner_timeout", "0", "retrotv_clock", "12", "retrotv_leveller", "loud",
                       "retrotv_static_sound", "off", "retrotv_hints", "off", "retrotv_theme", "phosphor",
                       "retrotv_static_style", "vhs", "retrotv_blip", "on", "retrotv_overscan", "10",
                       "retrotv_refresh", "50", "retrotv_zoom_default", "zoom", "retrotv_zoom_remember", "off",
                       "retrotv_resolution", "240", NULL };
  options_parse(&o, map_get, kv);
  CHECK(o.banner_secs == 0 && !o.clock24 && o.leveller == 2 && o.static_vol == 0 && !o.hints);
  CHECK(o.theme == 2 && o.static_vhs && o.blip);
  CHECK(o.overscan == 10 && o.refresh == 50 && o.zoom_default && !o.zoom_remember && o.res240);

  const char *bad[] = { "retrotv_banner_timeout", "99", "retrotv_leveller", "LOUD", NULL };
  options_parse(&o, map_get, bad);                    /* not a listed choice: default */
  CHECK(o.banner_secs == 4 && o.leveller == 1);

  for (int i = 0; i < options_count(); i++) {         /* keys unique and namespaced */
    CHECK(!strncmp(options_key(i), "retrotv_", 8));
    for (int k = i + 1; k < options_count(); k++) CHECK(strcmp(options_key(i), options_key(k)));
  }

  fe_version = 2;                                     /* v2 frontend */
  CHECK(options_register(fake_env) == 2);
  CHECK(got_v2 && got_v2->definitions[options_count()].key == NULL);
  CHECK(got_v2->definitions[0].values[0].value && got_v2->definitions[0].default_value);
  for (int i = 0; got_v2->categories[i].key; i++) {   /* every category is used */
    int used = 0;
    for (int k = 0; k < options_count(); k++) used |= !strcmp(got_v2->definitions[k].category_key, got_v2->categories[i].key);
    CHECK(used);
  }

  fe_version = 0;                                     /* old frontend: "Desc; default|others" */
  CHECK(options_register(fake_env) == 1);
  int ci = -1, ti = -1;
  for (int i = 0; got_v0[i].key; i++) {
    if (!strcmp(got_v0[i].key, "retrotv_clock")) ci = i;
    if (!strcmp(got_v0[i].key, "retrotv_theme")) ti = i;
  }
  CHECK(ci >= 0 && !strcmp(got_v0[ci].value, "Clock; 24|12"));
  CHECK(ti >= 0 && !strcmp(got_v0[ti].value, "Colour theme; navy|teletext|phosphor"));
  CHECK(got_v0[options_count()].key == NULL);

  options_read(&o, fake_env);                         /* reads through GET_VARIABLE */
  CHECK(!o.clock24 && o.banner_secs == 4);

  if (fails) return 1;
  printf("test_options: all passed (%d options)\n", options_count());
  return 0;
}
