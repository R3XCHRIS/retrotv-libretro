/* theme.h - colour themes for the on-screen boxes (banner, list, guide, ...).
 * The classic TV colours stay fixed: NO SIGNAL blue, error red, OSD green. */
#ifndef RETROTV_THEME_H
#define RETROTV_THEME_H

#include <stdint.h>

typedef struct {
  const char *name;
  uint32_t bg;          /* box background */
  uint32_t accent;      /* rules, headers, labels */
  uint32_t on_accent;   /* text drawn on an accent fill */
  uint32_t title;       /* channel numbers */
  uint32_t text;        /* main text */
  uint32_t sub;         /* secondary text: group, next, paths */
  uint32_t muted;       /* status line, hints */
  uint32_t cell;        /* list rows and guide cells */
  uint32_t cell_sel;    /* highlighted row */
  uint32_t cell_gap;    /* guide row without data */
  uint32_t bar;         /* progress bar track */
} rtv_theme;

enum { THEME_NAVY, THEME_TELETEXT, THEME_PHOSPHOR, THEME_COUNT };

extern const rtv_theme *TH;           /* current theme, never NULL */
void theme_set(int id);

#endif
