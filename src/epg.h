/* epg.h - programme guide from XMLTV (the playlist's x-tvg-url).
 * Pure C with no FFmpeg dependency: the core fetches the text, this parses it.
 * Only a window around "now" is kept, so a 17-day guide stays small. */
#ifndef RETROTV_EPG_H
#define RETROTV_EPG_H

#include <stddef.h>
#include <time.h>

typedef struct {
  time_t start, stop;      /* UTC */
  char *title;
  char *sub;               /* sub-title, may be NULL */
  char *desc;              /* may be NULL; shortened */
} epg_prog;

typedef struct {
  char *id;                /* XMLTV channel id = the playlist's tvg-id */
  epg_prog *p;             /* sorted by start */
  size_t n;
} epg_chan;

typedef struct {
  char *name;              /* a <display-name>, normalised (lowercase letters and digits) */
  char *id;                /* the <channel id> it belongs to */
} epg_alias;

typedef struct {
  epg_chan *ch;            /* sorted by id */
  size_t nch;
  size_t nprog;
  epg_alias *alias;        /* channel display names, for guides from another source */
  size_t nalias;
  int match_numbers;       /* channel number as id: only for the playlist's own guide (default 1) */
} epg;

/* Parse XMLTV text, keeping programmes that overlap [from, to).
 * Returns NULL on allocation failure; a guide with no programmes is valid. */
epg *epg_parse(const char *xml, time_t from, time_t to);
void epg_free(epg *g);

/* Programmes for a channel id (NULL if unknown). */
const epg_chan *epg_find(const epg *g, const char *id);

/* Programmes for a playlist channel: by tvg-id, then by name against the
 * guide's display names (for a guide from another source, whose ids don't
 * match the playlist), then by channel number if g->match_numbers. A guide
 * from elsewhere numbers its channels differently, so the core turns number
 * matching off for an epg= guide. Any argument may be NULL. */
const epg_chan *epg_lookup(const epg *g, const char *tvg_id, const char *number, const char *tvg_name, const char *name);

/* The programme on air at t and the one after it (either may be NULL). */
void epg_now_next(const epg_chan *c, time_t t, const epg_prog **now, const epg_prog **next);

/* "20261013190635 +0000" -> UTC seconds; -1 if malformed. */
time_t epg_time(const char *s);

/* UTF-8 text to printable ASCII for the 8x8 font: common punctuation
 * (curly quotes, dashes, middle dot, ellipsis) mapped, other non-ASCII dropped. */
void epg_ascii(const char *in, char *out, size_t n);

/* Shorten a title in place to max characters: drop a trailing " - S01E02"
 * first, then cut and remove a dangling separator (never ends in " -"). */
void epg_shorten(char *t, int max);

/* Clock text for t in local time: "20:40" (24 h) or "8:40" (12 h); with
 * ampm, the 12-hour form gets "pm"/"am" ("8:40pm"). */
void epg_clock(time_t t, int h24, int ampm, char *out, size_t n);

#endif
