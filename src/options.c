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
  { "picture", "Picture", "Video size, zoom and refresh rate." },
  { "sound", "Sound", "Volume leveller and tuning sounds." },
  { "interface", "Interface", "Banner, clock and on-screen hints." },
  { "remote", "Remote control", "Change channel from Zaparoo, a script or a phone." },
};

static const opt_def OPTS[] = {
  { "retrotv_overscan", "Overscan margin",
    "Shrink the video so its edges stay visible on screens that cut them off. Menus are always inside the safe area.",
    "picture", "0", { { "0", "None" }, { "5", "5%" }, { "10", "10%" } } },
  { "retrotv_zoom_default", "Picture",
    "Fit shows the whole picture with black bars; Zoom fills the 4:3 screen. Y switches while watching.",
    "picture", "fit", { { "fit", "Fit" }, { "zoom", "Zoom" } } },
  { "retrotv_zoom_remember", "Remember zoom per channel",
    "Keep each channel's Fit/Zoom choice from the Y button.",
    "picture", "on", { { "on", "On" }, { "off", "Off" } } },
  { "retrotv_dual", "Two screens (dual mode)",
    "Two CRTs: a 1280x480 picture (1280x240 with Output 240p) for ReplayOS Multi screen = Dual horizontal (set it for RetroTV only). Connect both screens before starting. This and Output apply when RetroTV restarts. Keeps 60 Hz. Turn screen 2 on and off in the X menu.",
    "picture", "off", { { "off", "Off" }, { "on", "On" } } },
  { "retrotv_resolution", "Output",
    "640x240 (240p) is progressive like old consoles on a CRT: no interlace flicker, half the vertical detail.",
    "picture", "480", { { "480", "640x480" }, { "240", "640x240 (240p)" } } },
  { "retrotv_refresh", "Refresh rate",
    "Auto switches to 50 Hz for 25/50 fps channels so motion is smooth. Lock it if your screen dislikes mode changes.",
    "picture", "auto", { { "auto", "Auto (follow the channel)" }, { "60", "60 Hz" }, { "50", "50 Hz" } } },
  { "retrotv_banner_timeout", "Banner timeout",
    "How long the channel banner stays up after a channel change. Off: only when you press A.",
    "interface", "4", { { "2", "2 seconds" }, { "4", "4 seconds" }, { "8", "8 seconds" }, { "0", "Off" } } },
  { "retrotv_clock", "Clock",
    "Clock on the banner and in the guide.",
    "interface", "24", { { "24", "24-hour" }, { "12", "12-hour" } } },
  { "retrotv_hints", "Button hints",
    "Show the button hints line on the banner.",
    "interface", "on", { { "on", "On" }, { "off", "Off" } } },
  { "retrotv_theme", "Colour theme",
    "Colours of the banner, channel list and guide.",
    "interface", "navy", { { "navy", "Cable Navy" }, { "teletext", "Teletext" }, { "phosphor", "Green Phosphor" } } },
  { "retrotv_static_style", "Tuning picture",
    "What you see while a channel tunes.",
    "interface", "classic", { { "classic", "Classic snow" }, { "vhs", "VHS" } } },
  { "retrotv_blip", "Channel-change sound",
    "A short click when you change channel, like an old TV.",
    "sound", "off", { { "off", "Off" }, { "on", "On" } } },
  { "retrotv_leveller", "Volume leveller",
    "Evens out loud and quiet channels. Loud aims higher, closer to game volume.",
    "sound", "normal", { { "normal", "Normal" }, { "loud", "Loud" }, { "off", "Off" } } },
  { "retrotv_static_sound", "Tuning static sound",
    "Volume of the hiss while a channel tunes.",
    "sound", "normal", { { "normal", "Normal" }, { "quiet", "Quiet" }, { "off", "Off" } } },
  { "retrotv_remote", "Remote control",
    "Tune by web request on port 7474, e.g. /tune?ch=36. This device only: Zaparoo and scripts on the Pi. Home network: also phones and computers on your network, with no password.",
    "remote", "local", { { "local", "This device only" }, { "lan", "Home network" }, { "off", "Off" } } },
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
  const char *t = val("retrotv_theme", get, ctx);
  o->theme = !strcmp(t, "teletext") ? 1 : !strcmp(t, "phosphor") ? 2 : 0;
  o->static_vhs = !strcmp(val("retrotv_static_style", get, ctx), "vhs");
  o->blip = !strcmp(val("retrotv_blip", get, ctx), "on");
  o->overscan = atoi(val("retrotv_overscan", get, ctx));
  const char *r = val("retrotv_refresh", get, ctx);
  o->refresh = !strcmp(r, "auto") ? 0 : atoi(r);
  o->zoom_default = !strcmp(val("retrotv_zoom_default", get, ctx), "zoom");
  o->zoom_remember = !strcmp(val("retrotv_zoom_remember", get, ctx), "on");
  o->res240 = !strcmp(val("retrotv_resolution", get, ctx), "240");
  o->dual = !strcmp(val("retrotv_dual", get, ctx), "on");
  const char *rc = val("retrotv_remote", get, ctx);
  o->remote = !strcmp(rc, "off") ? 0 : !strcmp(rc, "lan") ? 2 : 1;
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
