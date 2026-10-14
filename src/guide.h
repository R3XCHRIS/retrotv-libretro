/* guide.h - the TV guide screen: a live preview, an info pane for the
 * highlighted programme and a 90-minute grid of channels (90s cable style). */
#ifndef RETROTV_GUIDE_H
#define RETROTV_GUIDE_H

#include <stddef.h>
#include <time.h>

#include "epg.h"
#include "m3u.h"
#include "osd.h"

#define GUIDE_SPAN (90 * 60)       /* seconds shown across the grid */
#define GUIDE_STEP (30 * 60)       /* left/right step and ruler marks */
#define GUIDE_ROWS 7               /* channel rows on screen */

typedef struct {
  int x, w;                        /* pixels, clipped to the grid */
  const epg_prog *p;
} guide_cell;

/* Programmes of c overlapping [t0, t0 + span), laid out across [x0, x0 + w).
 * Returns the number of cells written (at most max). */
int guide_cells(const epg_chan *c, time_t t0, time_t span, int x0, int w, guide_cell *out, int max);

/* Start of the 30-minute slot containing t (local time, like a printed guide). */
time_t guide_slot(time_t t);

typedef struct {
  size_t sel;                      /* highlighted channel */
  size_t top;                      /* first channel row on screen */
  time_t t0;                       /* left edge of the grid */
  int clock24;                     /* 24 h times, else 12 h */
} guide_view;

/* Guide data for a playlist channel (tvg-id, number, then name). */
const epg_chan *guide_chan(const epg *g, const m3u_channel *c);

/* Keep sel on screen. */
void guide_scroll(guide_view *v, size_t count);

/* The programme the cursor is on: on the highlighted channel at max(now, t0). */
const epg_prog *guide_focus(const epg *g, const m3u_list *l, const guide_view *v, time_t now);

/* Draw the whole screen. The framebuffer's current picture (the live channel)
 * is shrunk into the preview window before being drawn over. */
void guide_draw(osd_surface *s, const m3u_list *l, const epg *g, const guide_view *v, size_t playing, time_t now);

#endif
