/* options.h - RetroTV's libretro core options (shown in the frontend's menu).
 * One table defines every option; it is registered as core options v2 when
 * the frontend supports it, else as the old v0 "Desc; a|b|c" variables. */
#ifndef RETROTV_OPTIONS_H
#define RETROTV_OPTIONS_H

#include "libretro.h"

typedef struct {
  double banner_secs;      /* 0 = only when asked (A) */
  int clock24;             /* 1 = 24 h, 0 = 12 h */
  int leveller;            /* 0 off, 1 normal, 2 loud */
  int static_vol;          /* tuning hiss: 0 off, 1 quiet, 2 normal */
  int hints;               /* button hints on the banner */
  int theme;               /* THEME_NAVY, THEME_TELETEXT, THEME_PHOSPHOR */
  int static_vhs;          /* tuning picture: 0 classic snow, 1 VHS */
  int pause_vhs;           /* paused picture: 1 VHS tape effect, 0 a clean still */
  int blip;                /* channel-change sound */
  int overscan;            /* video margin, percent per side: 0, 5, 10 */
  int refresh;             /* 0 auto (follow the stream), 50, 60 */
  int zoom_default;        /* 0 FIT, 1 ZOOM for channels without a remembered choice */
  int zoom_remember;       /* remember Y per channel */
  int res240;              /* output 640x240 (240p) instead of 640x480 */
  int remote;              /* remote control: 0 off, 1 this device only, 2 home network */
  int zaparoo;             /* tell Zaparoo Core on this device what's on */
  int update_check;        /* look for a newer RetroTV once a day */
  int dual;                /* two screens: 1280x480 for ReplayOS "Dual horizontal" (applied at start) */
} rtv_options;

/* Defaults, as declared in the table. */
void options_defaults(rtv_options *o);

/* Fill o from a key -> value lookup (NULL or unknown values keep the default). */
void options_parse(rtv_options *o, const char *(*get)(const char *key, void *ctx), void *ctx);

/* Declare the options to the frontend: returns 2 (v2), 1 (v0 variables) or 0 (refused). */
int options_register(retro_environment_t env);

/* Read the current values from the frontend into o. */
void options_read(rtv_options *o, retro_environment_t env);

/* Number of declared options and the key of the i-th (for tests). */
int options_count(void);
const char *options_key(int i);

#endif
