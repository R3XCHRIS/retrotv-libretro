#include "guide.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

/* layout, inside the CRT overscan-safe area (5% = 32 px / 24 px) */
#define L 32                       /* left edge */
#define R 608                      /* right edge */
#define PV_W 160                   /* preview window */
#define PV_H 120
#define TOP 24
#define RULER_Y 152
#define ROW_Y 174
#define ROW_H 38
#define CH_W 148                   /* channel column */
#define GX (L + CH_W + 4)          /* grid */
#define GW (R - GX)

#define NAVY TH->on_accent         /* text on accent fills (ruler, focused cell) */
#define BG TH->bg
#define GOLD TH->accent
#define CELL TH->cell
#define CELL_SEL TH->cell_sel
#define WHITE TH->text
#define DIM TH->sub
#define YELLOW TH->title

int guide_cells(const epg_chan *c, time_t t0, time_t span, int x0, int w, guide_cell *out, int max)
{
  int n = 0;
  time_t t1 = t0 + span;
  if (!c || span <= 0) return 0;
  for (size_t i = 0; i < c->n && n < max; i++) {
    const epg_prog *p = &c->p[i];
    if (p->stop <= t0 || p->start >= t1) continue;
    time_t a = p->start > t0 ? p->start : t0, b = p->stop < t1 ? p->stop : t1;
    int xa = x0 + (int)((long long)(a - t0) * w / span), xb = x0 + (int)((long long)(b - t0) * w / span);
    if (xb - xa < 2) continue;
    out[n++] = (guide_cell){ xa, xb - xa, p };
  }
  return n;
}

time_t guide_slot(time_t t)
{
  struct tm tmv;
  localtime_r(&t, &tmv);
  return t - (tmv.tm_min % 30) * 60 - tmv.tm_sec;
}

void guide_scroll(guide_view *v, size_t count)
{
  if (!count) { v->sel = v->top = 0; return; }
  if (v->sel >= count) v->sel = count - 1;
  if (v->sel < v->top) v->top = v->sel;
  if (v->sel >= v->top + GUIDE_ROWS) v->top = v->sel - GUIDE_ROWS + 1;
  if (count > GUIDE_ROWS && v->top > count - GUIDE_ROWS) v->top = count - GUIDE_ROWS;
}

const epg_chan *guide_chan(const epg *g, const m3u_channel *c)
{
  return epg_lookup(g, c->tvg_id, c->number, c->tvg_name, c->name);
}

const epg_prog *guide_focus(const epg *g, const m3u_list *l, const guide_view *v, time_t now)
{
  if (!l || v->sel >= l->count) return NULL;
  const epg_prog *p, *next;
  time_t t = now > v->t0 ? now : v->t0;
  epg_now_next(guide_chan(g, &l->ch[v->sel]), t, &p, &next);
  /* paged ahead and the programme on air at the left edge is nearly over:
   * focus the next one, like a printed guide's first full listing */
  if (p && next && v->t0 > now && p->stop - v->t0 < 10 * 60) p = next;
  return p ? p : next && next->start < v->t0 + GUIDE_SPAN ? next : NULL;
}

/* ASCII copy of s cut to fit max characters */
static void label(const char *s, char *out, size_t n, int max)
{
  epg_ascii(s ? s : "", out, n);
  epg_shorten(out, max);
}


/* word-wrap text into at most lines lines of cols characters */
static void wrap(osd_surface *s, int x, int y, const char *text, int cols, int lines, uint32_t rgb)
{
  const char *p = text;
  for (int ln = 0; ln < lines && *p; ln++) {
    while (*p == ' ') p++;
    int len = (int)strlen(p);
    int cut = len <= cols ? len : cols;
    if (len > cols) {
      int sp = cut;
      while (sp > 0 && p[sp] != ' ') sp--;
      if (sp > cols / 2) cut = sp;
    }
    char buf[96];
    if (cut > (int)sizeof(buf) - 4) cut = (int)sizeof(buf) - 4;
    memcpy(buf, p, (size_t)cut);
    buf[cut] = '\0';
    if (ln == lines - 1 && len > cut) strcat(buf, "...");
    osd_text(s, x, y + ln * 12, buf, 1, rgb);
    p += cut;
  }
}

static void draw_preview(osd_surface *s)
{
  /* shrink the 640x480 picture already in the framebuffer by 4 (nearest),
   * before anything is drawn over it */
  static uint32_t pv[PV_W * PV_H];
  for (int y = 0; y < PV_H; y++)
    for (int x = 0; x < PV_W; x++)
      pv[y * PV_W + x] = s->px[(size_t)(y * 4 + 2) * s->pitch + x * 4 + 2];
  osd_fill(s, 0, 0, s->w, s->h, BG, 255);
  osd_fill(s, L - 2, TOP - 2, PV_W + 4, PV_H + 4, GOLD, 255);
  for (int y = 0; y < PV_H; y++)
    memcpy(&s->px[(size_t)(TOP + y) * s->pitch + L], &pv[y * PV_W], PV_W * 4);
}

static void draw_info(osd_surface *s, const m3u_list *l, const epg_prog *p, const guide_view *v)
{
  int x = L + PV_W + 16, cols = (R - x) / 8;
  char buf[128], t0[12], t1[12];
  if (v->sel < l->count) {
    const m3u_channel *c = &l->ch[v->sel];
    char name[64];
    label(c->name, name, sizeof(name), 40);
    snprintf(buf, sizeof(buf), "%s  %s", c->number ? c->number : "", name);
    osd_text(s, x, TOP, buf, 1, GOLD);
  }
  if (!p) {
    osd_text_shadow(s, x, TOP + 20, "No information", 2, DIM);
    return;
  }
  label(p->title, buf, sizeof(buf), (R - x) / 16);
  osd_text_shadow(s, x, TOP + 18, buf, 2, WHITE);
  epg_clock(p->start, v->clock24, 1, t0, sizeof(t0));
  epg_clock(p->stop, v->clock24, 1, t1, sizeof(t1));
  char sub[96] = "";
  if (p->sub) label(p->sub, sub, sizeof(sub), cols - 16);
  snprintf(buf, sizeof(buf), "%s - %s  %s", t0, t1, sub);
  osd_text(s, x, TOP + 42, buf, 1, YELLOW);
  if (p->desc) {
    char d[512];
    epg_ascii(p->desc, d, sizeof(d));
    wrap(s, x, TOP + 60, d, cols, 5, DIM);
  }
}

static void draw_ruler(osd_surface *s, time_t t0, time_t now, int clock24)
{
  struct tm tmv;
  char day[24], t[12];
  localtime_r(&now, &tmv);
  strftime(day, sizeof(day), "%a %d %b", &tmv);
  for (char *c = day; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
  osd_fill(s, L, RULER_Y, R - L, 18, GOLD, 255);
  osd_text(s, L + 6, RULER_Y + 5, day, 1, NAVY);
  for (time_t m = t0; m < t0 + GUIDE_SPAN; m += GUIDE_STEP) {
    int x = GX + (int)((long long)(m - t0) * GW / GUIDE_SPAN);
    epg_clock(m, clock24, 1, t, sizeof(t));
    osd_fill(s, x, RULER_Y, 2, 18, NAVY, 255);
    osd_text(s, x + 6, RULER_Y + 5, t, 1, NAVY);
  }
}

void guide_draw(osd_surface *s, const m3u_list *l, const epg *g, const guide_view *v, size_t playing, time_t now)
{
  draw_preview(s);
  const epg_prog *focus = guide_focus(g, l, v, now);
  draw_info(s, l, focus, v);
  draw_ruler(s, v->t0, now, v->clock24);

  guide_cell cells[24];
  for (int r = 0; r < GUIDE_ROWS && v->top + (size_t)r < l->count; r++) {
    size_t i = v->top + (size_t)r;
    const m3u_channel *c = &l->ch[i];
    int y = ROW_Y + r * ROW_H, sel = i == v->sel;
    char num[8], name[24];
    osd_fill(s, L, y, CH_W, ROW_H - 2, sel ? CELL_SEL : CELL, 255);
    label(c->number, num, sizeof(num), 5);
    label(c->name, name, sizeof(name), (CH_W - 12) / 8);
    osd_text_shadow(s, L + 6, y + 4, num, 2, i == playing ? GOLD : YELLOW);
    osd_text(s, L + 6, y + 24, name, 1, WHITE);

    int n = guide_cells(guide_chan(g, c), v->t0, GUIDE_SPAN, GX, GW, cells, 24);
    osd_fill(s, GX, y, GW, ROW_H - 2, TH->cell_gap, 255);          /* gaps: no info */
    if (!n) osd_text(s, GX + 6, y + 15, "No information", 1, DIM);
    for (int k = 0; k < n; k++) {
      int hl = sel && cells[k].p == focus;
      osd_fill(s, cells[k].x + 1, y, cells[k].w - 2, ROW_H - 2, hl ? GOLD : sel ? CELL_SEL : CELL, 255);
      char t[96];
      int cols = (cells[k].w - 10) / 8;
      if (cols < 1) continue;
      label(cells[k].p->title, t, sizeof(t), cols);
      osd_text(s, cells[k].x + 5, y + 15, t, 1, hl ? NAVY : WHITE);
    }
  }
  if (now >= v->t0 && now < v->t0 + GUIDE_SPAN) {                        /* "now" line */
    int x = GX + (int)((long long)(now - v->t0) * GW / GUIDE_SPAN);
    osd_fill(s, x - 1, RULER_Y + 18, 2, GUIDE_ROWS * ROW_H, OSD_RGB(255, 80, 80), 255);
  }
  osd_text(s, L, ROW_Y + GUIDE_ROWS * ROW_H + 6, "UP/DN CH  LEFT/RIGHT TIME  A WATCH  B CLOSE", 1, DIM);
}
