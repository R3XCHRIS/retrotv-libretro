#include "options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXV 8

typedef struct { const char *value, *label; } opt_value;

typedef struct {
  const char *key, *desc, *info, *category, *def;
  opt_value v[MAXV];
} opt_def;

static const struct { const char *key, *desc, *info; } CATS[] = {
  { "sound", "Sound", "Volume leveller and tuning sounds." },
  { "interface", "Interface", "Banner, clock and on-screen hints." },
};

static const opt_def OPTS[] = {
  { "retrotv_banner_timeout", "Banner timeout",
    "How long the channel banner stays up after a channel change. Off: only when you press A.",
    "interface", "4", { { "2", "2 seconds" }, { "4", "4 seconds" }, { "8", "8 seconds" }, { "0", "Off" } } },
  { "retrotv_clock", "Clock",
    "Clock on the banner and in the guide.",
    "interface", "24", { { "24", "24-hour" }, { "12", "12-hour" } } },
  { "retrotv_hints", "Button hints",
    "Show the button hints line on the banner.",
    "interface", "on", { { "on", "On" }, { "off", "Off" } } },
  { "retrotv_leveller", "Volume leveller",
    "Evens out loud and quiet channels. Loud aims higher, closer to game volume.",
    "sound", "normal", { { "normal", "Normal" }, { "loud", "Loud" }, { "off", "Off" } } },
  { "retrotv_static_sound", "Tuning static sound",
    "Volume of the hiss while a channel tunes.",
    "sound", "normal", { { "normal", "Normal" }, { "quiet", "Quiet" }, { "off", "Off" } } },
};
#define NOPTS ((int)(sizeof(OPTS) / sizeof(OPTS[0])))
#define NCATS ((int)(sizeof(CATS) / sizeof(CATS[0])))

int options_count(void) { return NOPTS; }
const char *options_key(int i) { return i >= 0 && i < NOPTS ? OPTS[i].key : NULL; }

static const opt_def *find(const char *key)
{
  for (int i = 0; i < NOPTS; i++) if (!strcmp(OPTS[i].key, key)) return &OPTS[i];
  return NULL;
}

/* value if it is one of the option's choices, else the default */
static const char *valid(const opt_def *d, const char *v)
{
  for (int k = 0; v && k < MAXV && d->v[k].value; k++) if (!strcmp(d->v[k].value, v)) return v;
  return d->def;
}

static const char *val(const char *key, const char *(*get)(const char *, void *), void *ctx)
{
  const opt_def *d = find(key);
  return valid(d, get ? get(key, ctx) : NULL);
}

void options_parse(rtv_options *o, const char *(*get)(const char *key, void *ctx), void *ctx)
{
  o->banner_secs = atof(val("retrotv_banner_timeout", get, ctx));
  o->clock24 = !strcmp(val("retrotv_clock", get, ctx), "24");
  o->hints = !strcmp(val("retrotv_hints", get, ctx), "on");
  const char *l = val("retrotv_leveller", get, ctx);
  o->leveller = !strcmp(l, "off") ? 0 : !strcmp(l, "loud") ? 2 : 1;
  const char *s = val("retrotv_static_sound", get, ctx);
  o->static_vol = !strcmp(s, "off") ? 0 : !strcmp(s, "quiet") ? 1 : 2;
}

void options_defaults(rtv_options *o) { options_parse(o, NULL, NULL); }

int options_register(retro_environment_t env)
{
  unsigned version = 0;
  if (!env(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version)) version = 0;
  if (version >= 2) {
    static struct retro_core_option_v2_category cats[NCATS + 1];
    static struct retro_core_option_v2_definition defs[NOPTS + 1];
    for (int i = 0; i < NCATS; i++) cats[i] = (struct retro_core_option_v2_category){ CATS[i].key, CATS[i].desc, CATS[i].info };
    for (int i = 0; i < NOPTS; i++) {
      memset(&defs[i], 0, sizeof(defs[i]));
      defs[i].key = OPTS[i].key;
      defs[i].desc = OPTS[i].desc;
      defs[i].info = OPTS[i].info;
      defs[i].category_key = OPTS[i].category;
      defs[i].default_value = OPTS[i].def;
      for (int k = 0; k < MAXV && OPTS[i].v[k].value; k++) {
        defs[i].values[k].value = OPTS[i].v[k].value;
        defs[i].values[k].label = OPTS[i].v[k].label;
      }
    }
    static struct retro_core_options_v2 opts = { cats, defs };
    if (env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &opts)) return 2;
  }
  /* v0: "Description; default|other|other", the default first */
  static char text[NOPTS][512];
  static struct retro_variable vars[NOPTS + 1];
  for (int i = 0; i < NOPTS; i++) {
    int n = snprintf(text[i], sizeof(text[i]), "%s; %s", OPTS[i].desc, OPTS[i].def);
    for (int k = 0; k < MAXV && OPTS[i].v[k].value && n < (int)sizeof(text[i]); k++)
      if (strcmp(OPTS[i].v[k].value, OPTS[i].def))
        n += snprintf(text[i] + n, sizeof(text[i]) - (size_t)n, "|%s", OPTS[i].v[k].value);
    vars[i] = (struct retro_variable){ OPTS[i].key, text[i] };
  }
  vars[NOPTS] = (struct retro_variable){ NULL, NULL };
  return env(RETRO_ENVIRONMENT_SET_VARIABLES, vars) ? 1 : 0;
}

static retro_environment_t read_env;

static const char *env_get(const char *key, void *ctx)
{
  (void)ctx;
  struct retro_variable v = { key, NULL };
  return read_env && read_env(RETRO_ENVIRONMENT_GET_VARIABLE, &v) ? v.value : NULL;
}

void options_read(rtv_options *o, retro_environment_t env)
{
  read_env = env;
  options_parse(o, env_get, NULL);
}
