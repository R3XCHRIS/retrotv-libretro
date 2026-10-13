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
