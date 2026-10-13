/* retrotv.c - RetroTV libretro core: flip through IPTV channels from an M3U
 * like a cable box. Prototype for ReplayOS (Raspberry Pi 5). */
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#include "libretro.h"
#include "ffapi.h"
#include "epg.h"
#include "guide.h"
#include "lastch.h"
#include "loud.h"
#include "onboard.h"
#include "options.h"
#include "testcard.h"
#include "theme.h"
#include "m3u.h"
#include "osd.h"
#include "player.h"

#define RETROTV_VERSION "0.1.0"
/* Output refresh: 60 Hz, or 50 Hz while a 25/50 fps (PAL) channel plays so
 * every frame is held equally long. Audio frames per run follow the rate. */
#define AFRAMES_MAX ((size_t)(PLAYER_RATE / 50))   /* 960 stereo frames per run at 50 Hz */
_Static_assert(PLAYER_RATE % 50 == 0 && PLAYER_RATE % 60 == 0, "audio frames per video frame must be whole");
#define INFO_SECONDS 4.0           /* banner after pressing A when the timeout option is Off */
#define TOAST_SECONDS 2.0

static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static m3u_list chans;
static player *pl;
static uint32_t fb[PLAYER_W * PLAYER_H];
static int16_t abuf[AFRAMES_MAX * 2];
static size_t cur, prev;
static double banner_until;
static uint64_t frame_no;
static double fps_out = 60.0;      /* current refresh */
static double t_clock;             /* seconds of output since load */
static int refresh_refused;        /* frontend said no to SET_SYSTEM_AV_INFO */
static int zoom;                   /* TV aspect button: 0 FIT, 1 ZOOM */
static uint64_t audio_sent, audio_taken;   /* frames offered to / accepted by the frontend */
static int audio_logged;
static loud_state loud;            /* AGC on programme audio */
static rtv_options opts;           /* core options (frontend menu) */
static int options_version;        /* 2 = v2, 1 = v0 variables, 0 = frontend refused */
static char store_dir[512];        /* <storage>/retrotv when playlist.txt was found there */
static char last_path[1100];       /* where the last channel is remembered */
static int saved_idx = -1;         /* channel index the file currently holds */
static double played_at;           /* t_now() when the current channel started playing */
static atomic_int saving;          /* a background write is running */
static atomic_int save_errno;      /* last failed write (logged by retro_run), 0 = ok */
#define REMEMBER_AFTER 5.0         /* seconds of watching before a channel counts as "last" */

/* Programme guide: fetched and parsed on its own thread, swapped in under guide_mu. */
static pthread_mutex_t guide_mu = PTHREAD_MUTEX_INITIALIZER;
static epg *guide;                 /* NULL until the first fetch succeeds */
static pthread_t guide_thread;
static int guide_thread_live;
static atomic_int guide_stop;
static char guide_url[1024];
#define GUIDE_BACK (2 * 3600)          /* keep programmes from 2 h ago ... */
#define GUIDE_AHEAD (26 * 3600)        /* ... to 26 h ahead */
#define GUIDE_REFRESH (2 * 3600)       /* refetch every 2 h */
#define GUIDE_RETRY 60                 /* after a failed fetch */
static char guide_msg[160];            /* written by the guide thread, logged by retro_run */
static atomic_int guide_msg_ready;     /* (the frontend's log callback may not be thread-safe) */
static char toast[16];             /* big green TV-style message, e.g. "ZOOM" */
static double toast_until;
static uint32_t noise_seed = 12345;
static int list_open;
static int guide_open;              /* TV guide screen (Start) */
static guide_view gv;
static size_t list_cursor;
static uint16_t pad_prev;
static int logged_playing, logged_error, logged_attempt;
static char help_reason[160];      /* why no channels loaded (for the log) */
static ob_kind ob_why = OB_NO_FILE; /* ... and for the setup screen */
static char ob_err[160];           /* FFmpeg's error text for OB_FETCH */
static char ob_path[300];          /* where playlist.txt goes (or is) */
static int testcard_on;            /* built-in test card instead of a channel */
static double tc_t, tc_phase;
static int welcome_on;             /* first-run controls screen */
static int welcome_save;           /* dismissed: write the 'seen' file (retried until it starts) */
static int blip_left;              /* audio frames of channel-change click still to play */
#define BLIP_FRAMES (PLAYER_RATE / 25)   /* 40 ms */
static char state_dir[1024];       /* where the core keeps its small files */

/* ---------- helpers ---------- */

static void logf_(enum retro_log_level lvl, const char *fmt, ...)
{
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (log_cb) log_cb(lvl, "[RetroTV] %s\n", buf);
  else fprintf(stderr, "[RetroTV] %s\n", buf);
  /* ReplayOS ships with logging off; keep our own small log in tmpfs */
  FILE *f = fopen("/tmp/retrotv.log", "a");
  if (f) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char ts[16];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    fprintf(f, "%s %s\n", ts, buf);
    fclose(f);
  }
}

static double t_now(void) { return t_clock; }
static size_t aframes(void) { return (size_t)(PLAYER_RATE / fps_out); }

/* One line per channel visit: frames shown and dropped, A/V skew. */
static void log_stats(void)
{
  player_stats st;
  if (!pl || !chans.count || !logged_playing) return;
  player_get_stats(pl, &st);
  long counted = st.shown - st.dropped;
  logf_(RETRO_LOG_INFO, "ch %s stats: %ld frames shown, %ld dropped, A/V skew avg %.1f ms max %.1f ms, audio gain %+.1f dB",
        chans.ch[cur].number, counted, st.dropped,
        st.shown ? 1000.0 * st.skew_sum / st.shown : 0.0, 1000.0 * st.skew_max,
        loud.gain > 0 ? 20.0 * log10(loud.gain) : 0.0);
}

static void tune(size_t idx)
{
  if (!chans.count) return;
  log_stats();
  idx %= chans.count;
  if (idx != cur) prev = cur;
  cur = idx;
  banner_until = t_now() + opts.banner_secs;
  logged_playing = logged_error = 0;
  logged_attempt = 1;
  logf_(RETRO_LOG_INFO, "tune ch %s \"%s\"", chans.ch[cur].number, chans.ch[cur].name);
  loud_reset(&loud);
  if (opts.blip) blip_left = BLIP_FRAMES;
  player_open(pl, chans.ch[cur].url);
}

static void clip_copy(char *dst, size_t n, const char *src, size_t maxchars)
{
  char *a = m3u_ascii(src);
  snprintf(dst, n, "%.*s", (int)maxchars, a ? a : "");
  free(a);
}

/* ---------- drawing ---------- */

static void hhmm(time_t t, char *out, size_t n) { epg_clock(t, opts.clock24, 0, out, n); }

static void draw_banner(osd_surface *s, player_state st)
{
  const m3u_channel *c = &chans.ch[cur];
  int bx = 24, by = PLAYER_H - 132, bw = PLAYER_W - 48, bh = 104;
  osd_fill(s, bx, by, bw, bh, TH->bg, 255);   /* solid, like a real cable box */
  osd_fill(s, bx, by, bw, 3, TH->accent, 255);          /* gold rule */

  char num[16], name[40], group[48], line[96];
  clip_copy(num, sizeof(num), c->number, 6);
  clip_copy(name, sizeof(name), c->name, 26);
  clip_copy(group, sizeof(group), c->group ? c->group : "", 40);

  /* Now / Next from the guide, if it has this channel */
  const char *gid = c->tvg_id && *c->tvg_id ? c->tvg_id : c->number;
  char now_t[48] = "", next_t[48] = "", t0[12] = "", t1[12] = "", t2[12] = "";
  double frac = -1;
  pthread_mutex_lock(&guide_mu);
  const epg_prog *pn, *px;
  time_t tnow = time(NULL);
  epg_now_next(epg_find(guide, gid), tnow, &pn, &px);
  if (pn) {
    epg_ascii(pn->title, now_t, sizeof(now_t));
    hhmm(pn->start, t0, sizeof(t0));
    hhmm(pn->stop, t1, sizeof(t1));
    frac = (double)(tnow - pn->start) / (double)(pn->stop - pn->start);
  }
  if (px) {
    epg_ascii(px->title, next_t, sizeof(next_t));
    hhmm(px->start, t2, sizeof(t2));
  }
  pthread_mutex_unlock(&guide_mu);
  int has_guide = pn || px;
  if (has_guide) {                                   /* taller banner, same bottom edge */
    bh = 160;
    by = PLAYER_H - 28 - bh;
    osd_fill(s, bx, by, bw, bh, TH->bg, 255);
    osd_fill(s, bx, by, bw, 3, TH->accent, 255);
  }

  osd_text_shadow(s, bx + 14, by + 16, num, 4, TH->title);
  int nx = bx + 14 + osd_text_width(num, 4) + 18;
  osd_text_shadow(s, nx, by + 18, name, 2, TH->text);
  if (*group) osd_text(s, nx, by + 42, group, 1, TH->sub);

  time_t tt = time(NULL);
  struct tm tmv;
  localtime_r(&tt, &tmv);
  char clk[12];
  epg_clock(tt, opts.clock24, 1, clk, sizeof(clk));
  osd_text_shadow(s, bx + bw - osd_text_width(clk, 2) - 14, by + 18, clk, 2, TH->text);

  switch (st) {
  case PLAYER_CONNECTING:
  case PLAYER_BUFFERING: {
    int dots = (int)(frame_no / 20) % 4;
    int att = player_attempt(pl, NULL, 0);
    char retry[24] = "";
    if (att > 1) snprintf(retry, sizeof(retry), "  (retry %d)", att);
    snprintf(line, sizeof(line), "TUNING%.*s%*s %.0fs%s", dots, "...", 3 - dots, "", player_tuning_seconds(pl), retry);
    break;
  }
  case PLAYER_ERROR: {
    char e[64];
    clip_copy(e, sizeof(e), player_error(pl), 56);
    snprintf(line, sizeof(line), "NO SIGNAL - %s", e);
    break;
  }
  case PLAYER_PLAYING: {
    player_info in;
    player_get_info(pl, &in);
    snprintf(line, sizeof(line), "%dx%d %s %.2ffps  %s", in.width, in.height, in.vcodec, in.fps, in.acodec);
    break;
  }
  default:
    line[0] = '\0';
  }
  int right = bx + bw - 14, y = by + 60;
  if (has_guide) {
    int lx = nx + 48, chars = (right - lx) / 16;      /* titles at 2x: 16 px per character */
    if (pn) {
      osd_text(s, nx, y + 4, "NOW", 1, TH->accent);
      epg_shorten(now_t, chars);
      osd_text_shadow(s, lx, y, now_t, 2, TH->text);
      int bx0 = lx + 48, bw0 = right - 48 - bx0;      /* "19:40 [=====-----] 20:10" */
      osd_text(s, lx, y + 22, t0, 1, TH->muted);
      osd_fill(s, bx0, y + 22, bw0, 8, TH->bar, 255);
      if (frac > 0) osd_fill(s, bx0, y + 22, (int)(bw0 * (frac > 1 ? 1 : frac)), 8, TH->accent, 255);
      osd_text(s, right - 40, y + 22, t1, 1, TH->muted);
    }
    if (px) {
      osd_text(s, nx, y + 42, "NEXT", 1, TH->accent);
      char nl[64];
      epg_shorten(next_t, (right - lx) / 8 - 7);            /* "20:40  " in front */
      snprintf(nl, sizeof(nl), "%s  %s", t2, next_t);
      osd_text(s, lx, y + 42, nl, 1, TH->sub);
    }
    y += 62;
  } else {
    y += 6;
  }
  int maxc = (right - nx) / 8;                       /* keep inside the banner */
  if (maxc > 0 && (int)strlen(line) > maxc) line[maxc] = '\0';
  osd_text(s, nx, y, line, 1, st == PLAYER_ERROR ? OSD_RGB(255, 120, 120) : TH->muted);
  if (opts.hints) osd_text(s, nx, y + 16, "A INFO  B LAST  Y ZOOM  SEL LIST  START GUIDE", 1, TH->muted);
}

static void draw_list(osd_surface *s)
{
  int rows = 12, x = 40, y = 40, w = PLAYER_W - 80, rh = 30;
  osd_fill(s, x, y, w, 36 + rows * rh, TH->bg, 230);
  osd_fill(s, x, y, w, 30, TH->accent, 255);
  osd_text(s, x + 12, y + 8, "CHANNEL LIST", 2, TH->on_accent);
  size_t first = list_cursor >= (size_t)rows / 2 ? list_cursor - (size_t)rows / 2 : 0;
  if (chans.count > (size_t)rows && first + (size_t)rows > chans.count) first = chans.count - (size_t)rows;
  for (int r = 0; r < rows && first + (size_t)r < chans.count; r++) {
    size_t i = first + (size_t)r;
    int ry = y + 36 + r * rh;
    if (i == list_cursor) osd_fill(s, x + 4, ry - 4, w - 8, rh - 2, TH->cell_sel, 255);
    char num[16], name[40];
    clip_copy(num, sizeof(num), chans.ch[i].number, 6);
    clip_copy(name, sizeof(name), chans.ch[i].name, 30);
    osd_text(s, x + 14, ry + 2, num, 2, i == cur ? TH->title : TH->sub);
    osd_text(s, x + 120, ry + 2, name, 2, TH->text);
  }
}

static void draw_no_signal(osd_surface *s)
{
  osd_fill(s, 0, 0, PLAYER_W, PLAYER_H, OSD_RGB(16, 32, 160), 255);   /* classic blue screen */
  const char *t = "NO SIGNAL";
  osd_text_shadow(s, (PLAYER_W - osd_text_width(t, 4)) / 2, 150, t, 4, TH->text);
}

/* word-wrap at 1x into lines of cols characters */
static int wrap1(osd_surface *s, int x, int y, const char *t, int cols, uint32_t rgb)
{
  char line[96];
  while (*t) {
    while (*t == ' ') t++;
    int len = (int)strlen(t), cut = len <= cols ? len : cols;
    if (len > cols) { int sp = cut; while (sp > 0 && t[sp] != ' ') sp--; if (sp > 0) cut = sp; }
    if (cut > (int)sizeof(line) - 1) cut = (int)sizeof(line) - 1;
    memcpy(line, t, (size_t)cut);
    line[cut] = '\0';
    osd_text(s, x, y, line, 1, rgb);
    y += 12;
    t += cut;
  }
  return y;
}

/* Setup screen when there are no channels: what's wrong, how to fix it. */
static void draw_help(osd_surface *s)
{
  char problem[96], hint[256];
  onboard_explain(ob_why, ob_err, problem, sizeof(problem), hint, sizeof(hint));
  osd_fill(s, 0, 0, PLAYER_W, PLAYER_H, TH->bg, 255);
  osd_fill(s, 32, 28, PLAYER_W - 64, 36, TH->accent, 255);
  osd_text(s, 48, 38, "RETROTV - SET UP YOUR CHANNELS", 2, TH->on_accent);
  osd_text_shadow(s, 48, 84, problem, 2, OSD_RGB(255, 140, 140));
  int y = wrap1(s, 48, 108, hint, 68, TH->text) + 14;
  osd_text(s, 48, y, "1. Put your playlist link on the first line of:", 1, TH->accent);
  y = wrap1(s, 72, y + 16, ob_path, 65, TH->sub) + 10;
  osd_text(s, 48, y, "2. For example, from Dispatcharr:", 1, TH->accent);
  osd_text(s, 72, y + 16, "http://<server-ip>:9191/output/m3u/<profile>", 1, TH->sub);
  osd_text(s, 48, y + 42, "3. Close and reopen RetroTV.", 1, TH->accent);
  osd_fill(s, 32, 400, PLAYER_W - 64, 2, TH->accent, 255);
  osd_text(s, 48, 414, "A: TEST CARD - check picture, sound and stereo", 1, TH->text);
  osd_text(s, 48, 430, "Settings: ReplayOS menu > System Settings", 1, TH->muted);
}

/* First launch: the controls, once. */
static void draw_welcome(osd_surface *s)
{
  int x = 64, y = 60, w = PLAYER_W - 128, h = 340;
  osd_fill(s, x, y, w, h, TH->bg, 235);
  osd_fill(s, x, y, w, 3, TH->accent, 255);
  osd_text_shadow(s, x + 24, y + 18, "WELCOME TO RETROTV", 2, TH->title);
  static const char *rows[][2] = {
    { "UP / DOWN", "change channel" }, { "A", "what's on (banner)" }, { "B", "last channel" },
    { "Y", "zoom / fit picture" }, { "START", "TV guide" }, { "SELECT", "channel list" },
  };
  for (int i = 0; i < 6; i++) {
    osd_text_shadow(s, x + 24, y + 58 + i * 34, rows[i][0], 2, TH->accent);
    osd_text(s, x + 200, y + 62 + i * 34, rows[i][1], 1, TH->text);
  }
  osd_text(s, x + 24, y + 270, "Settings: ReplayOS menu > System Settings", 1, TH->sub);
  osd_text_shadow(s, x + 24, y + 298, "Press A to start watching", 2, TH->text);
}

/* ---------- playlist lookup ---------- */

static char *slurp(const char *path, size_t max)
{
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  char *buf = malloc(max + 1);
  size_t n = buf ? fread(buf, 1, max, f) : 0;
  fclose(f);
  if (!buf) return NULL;
  buf[n] = '\0';
  return buf;
}

static char *fetch_url(const char *url, size_t max, const AVIOInterruptCB *icb, char *err, size_t errlen)
{
  AVIOContext *io = NULL;
  AVDictionary *o = NULL;
  p_av_dict_set(&o, "rw_timeout", "10000000", 0);
  p_av_dict_set(&o, "user_agent", "RetroTV/" RETROTV_VERSION " (libretro)", 0);
  int rc = p_avio_open2(&io, url, AVIO_FLAG_READ, icb, &o);
  p_av_dict_free(&o);
  if (rc < 0) { char e[96]; p_av_strerror(rc, e, sizeof(e)); snprintf(err, errlen, "fetch failed: %s", e); return NULL; }
  char *buf = malloc(max + 1);
  size_t len = 0;
  while (buf && len < max) {
    int n = p_avio_read(io, (unsigned char *)buf + len, (int)(max - len));
    if (n <= 0) break;
    len += (size_t)n;
  }
  p_avio_closep(&io);
  if (buf) buf[len] = '\0';
  return buf;
}

/* First line that isn't blank or a comment, trimmed (in place). */
static char *first_line(char *t)
{
  for (char *line = strtok(t, "\r\n"); line; line = strtok(NULL, "\r\n")) {
    while (*line == ' ' || *line == '\t') line++;
    char *e = line + strlen(line);
    while (e > line && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
    if (*line && *line != '#') return line;
  }
  return NULL;
}

static int ends_with(const char *s, const char *suf)
{
  size_t a = strlen(s), b = strlen(suf);
  return a >= b && !strcasecmp(s + a - b, suf);
}

/* Where the channel list comes from, in order:
 *   1. the launched file itself if it is .m3u/.m3u8
 *   2. first line of the launched file (.lr/.txt) - URL or path
 *   3. <storage>/retrotv/playlist.txt (first line), storage = nfs, usb, nvme, sd
 *   4. <storage>/retrotv/channels.m3u                                             */
static int resolve_playlist(const char *game_path, char *out, size_t n)
{
  char buf[1024];
  if (game_path && (ends_with(game_path, ".m3u") || ends_with(game_path, ".m3u8"))) {
    snprintf(out, n, "%s", game_path);
    return 0;
  }
  if (game_path) {
    char *t = slurp(game_path, 4096), *l = t ? first_line(t) : NULL;
    if (l) {
      snprintf(ob_path, sizeof(ob_path), "%s", game_path);    /* the file the link came from */
      if (m3u_is_url(l) || l[0] == '/') snprintf(out, n, "%s", l);
      else {
        snprintf(buf, sizeof(buf), "%s", game_path);
        char *slash = strrchr(buf, '/');
        if (slash) *slash = '\0';
        snprintf(out, n, "%s/%s", slash ? buf : ".", l);
      }
      free(t);
      return 0;
    }
    free(t);
  }
  static const char *roots[] = { "/media/nfs", "/media/usb", "/media/nvme", "/media/sd" };
  for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
    snprintf(buf, sizeof(buf), "%s/retrotv/playlist.txt", roots[i]);
    char *t = slurp(buf, 4096), *l = t ? first_line(t) : NULL;
    if (t && !l && ob_why != OB_EMPTY_FILE) { ob_why = OB_EMPTY_FILE; snprintf(ob_path, sizeof(ob_path), "%s", buf); }
    if (l) {
      snprintf(store_dir, sizeof(store_dir), "%s/retrotv", roots[i]);
      snprintf(ob_path, sizeof(ob_path), "%s", buf);
      if (m3u_is_url(l) || l[0] == '/') snprintf(out, n, "%s", l);
      else snprintf(out, n, "%s/retrotv/%s", roots[i], l);
      free(t);
      return 0;
    }
    free(t);
    snprintf(buf, sizeof(buf), "%s/retrotv/channels.m3u", roots[i]);
    FILE *f = fopen(buf, "rb");
    if (f) {
      fclose(f);
      snprintf(store_dir, sizeof(store_dir), "%s/retrotv", roots[i]);
      snprintf(out, n, "%s", buf);
      return 0;
    }
  }
  if (ob_why != OB_EMPTY_FILE) {                     /* suggest the first mounted storage */
    ob_why = OB_NO_FILE;
    snprintf(ob_path, sizeof(ob_path), "<storage>/retrotv/playlist.txt");
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
      struct stat st;
      snprintf(buf, sizeof(buf), "%s/roms", roots[i]);
      if (!stat(buf, &st) && S_ISDIR(st.st_mode)) { snprintf(ob_path, sizeof(ob_path), "%s/retrotv/playlist.txt", roots[i]); break; }
    }
  }
  return -1;
}

static int load_playlist(const char *src)
{
  if (m3u_is_url(src)) {
    char err[128] = "";
    char *text = fetch_url(src, 8u << 20, NULL, err, sizeof(err));
    if (!text) { ob_why = OB_FETCH; snprintf(ob_err, sizeof(ob_err), "%s", err); snprintf(help_reason, sizeof(help_reason), "%s", err); return -1; }
    if (!onboard_is_m3u(text)) { ob_why = OB_NOT_M3U; snprintf(help_reason, sizeof(help_reason), "not an M3U playlist"); free(text); return -1; }
    int rc = m3u_parse(text, NULL, &chans);
    free(text);
    return rc;
  }
  if (m3u_load(src, &chans)) {
    ob_why = OB_FETCH;
    snprintf(ob_err, sizeof(ob_err), "cannot read %s", src);
    snprintf(help_reason, sizeof(help_reason), "cannot read %s", src);
    return -1;
  }
  return 0;
}

/* ---------- programme guide ---------- */

static void guide_note(const char *fmt, ...)
{
  if (atomic_load(&guide_msg_ready)) return;       /* previous one not logged yet: drop */
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(guide_msg, sizeof(guide_msg), fmt, ap);
  va_end(ap);
  atomic_store(&guide_msg_ready, 1);
}

static int guide_interrupt(void *opaque) { (void)opaque; return atomic_load(&guide_stop); }

/* sleep up to secs, waking early when asked to stop */
static void guide_sleep(int secs)
{
  for (int i = 0; i < secs * 10 && !atomic_load(&guide_stop); i++) {
    struct timespec ts = { 0, 100 * 1000000L };
    nanosleep(&ts, NULL);
  }
}

static void *guide_main(void *arg)
{
  (void)arg;
  const AVIOInterruptCB icb = { guide_interrupt, NULL };
  while (!atomic_load(&guide_stop)) {
    char err[128] = "";
    char *xml = fetch_url(guide_url, 32u << 20, &icb, err, sizeof(err));
    epg *g = NULL;
    if (xml) {
      time_t now = time(NULL);
      g = epg_parse(xml, now - GUIDE_BACK, now + GUIDE_AHEAD);
      free(xml);
    }
    if (atomic_load(&guide_stop)) { epg_free(g); break; }
    if (!g) {
      guide_note("guide fetch failed (%s); retrying in %d s", *err ? err : "parse error", GUIDE_RETRY);
      guide_sleep(GUIDE_RETRY);
      continue;
    }
    guide_note("guide: %zu programmes on %zu channels", g->nprog, g->nch);
    pthread_mutex_lock(&guide_mu);
    epg *old = guide;
    guide = g;
    pthread_mutex_unlock(&guide_mu);
    epg_free(old);
    guide_sleep(GUIDE_REFRESH);
  }
  return NULL;
}

static void guide_start(const char *url)
{
  if (!url || !*url || guide_thread_live) return;
  snprintf(guide_url, sizeof(guide_url), "%s", url);
  atomic_store(&guide_stop, 0);
  guide_thread_live = !pthread_create(&guide_thread, NULL, guide_main, NULL);
}

static void guide_shutdown(void)
{
  if (guide_thread_live) {
    atomic_store(&guide_stop, 1);                   /* aborts a fetch in progress too */
    pthread_join(guide_thread, NULL);
    guide_thread_live = 0;
  }
  pthread_mutex_lock(&guide_mu);
  epg_free(guide);
  guide = NULL;
  pthread_mutex_unlock(&guide_mu);
}

/* ---------- last channel ---------- */

/* <save dir>/retrotv-last-<hash>.txt; the frontend's save directory if it has
 * one, else <storage>/retrotv (survives ReplayOS updates), else /tmp. */
static void last_init(const char *src)
{
  const char *dir = NULL;
  if (!env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) || !dir || !*dir) dir = NULL;
  if (!dir) dir = *store_dir ? store_dir : "/tmp";
  snprintf(state_dir, sizeof(state_dir), "%s", dir);
  snprintf(last_path, sizeof(last_path), "%s/retrotv-last-%08x.txt", dir, (unsigned)lastch_hash(src));
  logf_(RETRO_LOG_INFO, "last channel file: %s", last_path);
}

/* mkdir -p for the directory part of path (frontends may hand us a save
 * directory that doesn't exist yet: ReplayOS does for the avtest slot) */
static void make_parent_dirs(const char *path)
{
  char d[1100];
  snprintf(d, sizeof(d), "%s", path);
  char *slash = strrchr(d, '/');
  if (!slash || slash == d) return;
  *slash = '\0';
  for (char *p = d + 1; *p; p++) {
    if (*p != '/') continue;
    *p = '\0';
    mkdir(d, 0755);
    *p = '/';
  }
  mkdir(d, 0755);
}

static int write_file(const char *path, const char *text)
{
  make_parent_dirs(path);
  char tmp[1200];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if (!f) return -1;
  int ok = fputs(text, f) >= 0;
  ok = (fclose(f) == 0) && ok;
  return ok ? rename(tmp, path) : (remove(tmp), -1);
}

typedef struct { char path[1100]; char text[400]; } save_job;

static void *save_main(void *arg)
{
  save_job *j = arg;
  if (write_file(j->path, j->text)) atomic_store(&save_errno, errno ? errno : EIO);
  free(j);
  atomic_store(&saving, 0);
  return NULL;
}

/* Storage may be NFS: write on a detached thread so retro_run never waits. */
/* Write a small file on a detached thread (one at a time); 0 if started. */
static int save_text_async(const char *path, const char *text)
{
  if (!*path || atomic_exchange(&saving, 1)) return -1;       /* busy: caller retries later */
  save_job *j = malloc(sizeof(*j));
  pthread_t t;
  pthread_attr_t a;
  int ok = 0;
  if (j && !pthread_attr_init(&a)) {
    snprintf(j->path, sizeof(j->path), "%s", path);
    snprintf(j->text, sizeof(j->text), "%s", text);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    ok = !pthread_create(&t, &a, save_main, j);
    pthread_attr_destroy(&a);
  }
  if (!ok) { free(j); atomic_store(&saving, 0); return -1; }
  return 0;
}

static void last_save_async(size_t idx)
{
  char text[400];
  lastch_format(&chans.ch[idx], text, sizeof(text));
  if (!save_text_async(last_path, text)) saved_idx = (int)idx;
}

static void welcome_path(char *out, size_t n) { snprintf(out, n, "%s/retrotv-welcome.txt", state_dir); }

/* ---------- audio ---------- */

/* Hand this run's audio to the frontend; once, after 10 s, log how much it
 * took and whether it reports audio as enabled (diagnostics for frontends
 * that stay silent). */
/* old-TV channel-change click: a decaying 1.2 kHz burst mixed over whatever plays */
static void mix_blip(size_t n)
{
  for (size_t i = 0; i < n && blip_left > 0; i++, blip_left--) {
    double t = (double)(BLIP_FRAMES - blip_left) / PLAYER_RATE;
    double v = sin(2.0 * M_PI * 1200.0 * t) * exp(-t * 90.0) * 9000.0;
    for (int c = 0; c < 2; c++) {
      int x = abuf[2 * i + c] + (int)v;
      abuf[2 * i + c] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
    }
  }
}

static void send_audio(void)
{
  size_t n = aframes();
  mix_blip(n);
  audio_sent += n;
  audio_taken += audio_batch_cb(abuf, n);
  if (!audio_logged && t_now() > 10.0) {
    int av = -1;
    if (!env_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av)) av = -1;
    int peak = 0;
    for (size_t i = 0; i < n * 2; i++) if (abs(abuf[i]) > peak) peak = abs(abuf[i]);
    logf_(RETRO_LOG_INFO, "audio: frontend took %llu of %llu frames; av_enable=%d (2 = audio on, 8 = hard-disabled, -1 = not supported); last peak %d",
          (unsigned long long)audio_taken, (unsigned long long)audio_sent, av, peak);
    audio_logged = 1;
  }
}

/* ---------- core options ---------- */

static void apply_options(void)
{
  options_read(&opts, env_cb);
  loud.target_db = opts.leveller == 2 ? LOUD_TARGET_LOUD_DB : LOUD_TARGET_DB;
  theme_set(opts.theme);
  gv.clock24 = opts.clock24;
}

/* the user changed something in the frontend's menu */
static void check_options(void)
{
  bool upd = false;
  if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &upd) && upd) {
    apply_options();
  logf_(RETRO_LOG_INFO, "core options: %s", options_version == 2 ? "v2" : options_version == 1 ? "v0 variables" : "not supported by frontend");
    logf_(RETRO_LOG_INFO, "options: banner %.0fs, clock %s, leveller %s, static %d, hints %s", opts.banner_secs,
          opts.clock24 ? "24h" : "12h", opts.leveller == 2 ? "loud" : opts.leveller ? "normal" : "off",
          opts.static_vol, opts.hints ? "on" : "off");
  }
}

/* ---------- refresh and aspect ---------- */

static void draw_toast(osd_surface *s)
{
  osd_text_shadow(s, 48, 40, toast, 4, OSD_RGB(80, 255, 80));
}

static void show_toast(const char *t)
{
  snprintf(toast, sizeof(toast), "%s", t);
  toast_until = t_now() + TOAST_SECONDS;
}

/* Ask the frontend for 50 Hz on 25/50 fps channels and 60 Hz otherwise. */
static void follow_refresh(double stream_fps)
{
  if (refresh_refused || stream_fps <= 0) return;
  double want = (fabs(stream_fps - 25.0) < 0.6 || fabs(stream_fps - 50.0) < 1.0) ? 50.0 : 60.0;
  if (want == fps_out) return;
  double was = fps_out;
  fps_out = want;
  struct retro_system_av_info av;
  retro_get_system_av_info(&av);
  if (!env_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av)) {
    fps_out = was;
    refresh_refused = 1;
    logf_(RETRO_LOG_WARN, "frontend refused %.0f Hz; staying at %.0f Hz", want, was);
    return;
  }
  logf_(RETRO_LOG_INFO, "refresh %.0f Hz -> %.0f Hz for a %.2f fps stream", was, want, stream_fps);
}

/* ---------- input ---------- */

static uint16_t read_pad(void)
{
  uint16_t m = 0;
  static const unsigned ids[] = {
    RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_A,
    RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_L,
    RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_X,
    RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
    RETRO_DEVICE_ID_JOYPAD_Y,
  };
  for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, ids[i])) m |= (uint16_t)(1u << ids[i]);
  return m;
}

static void handle_input(void)
{
  uint16_t pad = read_pad();
  uint16_t down = pad & (uint16_t)~pad_prev;
  pad_prev = pad;
#define PRESSED(id) (down & (1u << RETRO_DEVICE_ID_JOYPAD_##id))
/* Face buttons by the label on an Xbox-style pad. ReplayOS maps pads by
 * position (libretro's SNES layout), so the bottom button "A" arrives as
 * JOYPAD_B, right "B" as JOYPAD_A, left "X" as JOYPAD_Y and top "Y" as JOYPAD_X. */
#define PAD_A B
#define PAD_B A
#define PAD_Y X
#define FACE(btn) PRESSED(btn)    /* expands PAD_x before PRESSED pastes it */
  if (testcard_on) {
    if (FACE(PAD_A) || FACE(PAD_B) || PRESSED(START)) testcard_on = 0;
    return;
  }
  if (!chans.count) {
    if (FACE(PAD_A)) { testcard_on = 1; tc_t = 0; }
    return;
  }
  if (welcome_on) {
    if (FACE(PAD_A) || FACE(PAD_B) || PRESSED(START)) { welcome_on = 0; welcome_save = 1; }
    return;
  }
  if (guide_open) {
    time_t now = time(NULL), first = guide_slot(now) - GUIDE_STEP, last = now + 24 * 3600;
    if (PRESSED(UP))    gv.sel = gv.sel ? gv.sel - 1 : chans.count - 1;
    if (PRESSED(DOWN))  gv.sel = (gv.sel + 1) % chans.count;
    if (PRESSED(LEFT) && gv.t0 - GUIDE_STEP >= first) gv.t0 -= GUIDE_STEP;
    if (PRESSED(RIGHT) && gv.t0 + GUIDE_STEP <= last) gv.t0 += GUIDE_STEP;
    guide_scroll(&gv, chans.count);
    if (FACE(PAD_A)) { guide_open = 0; if (gv.sel != cur) tune(gv.sel); }
    if (FACE(PAD_B) || PRESSED(START)) guide_open = 0;
    return;
  }
  if (list_open) {
    if (PRESSED(UP))   list_cursor = list_cursor ? list_cursor - 1 : chans.count - 1;
    if (PRESSED(DOWN)) list_cursor = (list_cursor + 1) % chans.count;
    if (FACE(PAD_A))   { list_open = 0; tune(list_cursor); }
    if (FACE(PAD_B) || PRESSED(SELECT)) list_open = 0;
    return;
  }
  if (PRESSED(UP) || PRESSED(R))   tune((cur + 1) % chans.count);
  if (PRESSED(DOWN) || PRESSED(L)) tune(cur ? cur - 1 : chans.count - 1);
  if (FACE(PAD_B))     tune(prev);
  if (FACE(PAD_A))     banner_until = t_now() + (opts.banner_secs > 0 ? opts.banner_secs : INFO_SECONDS);
  if (PRESSED(SELECT)) { list_open = 1; list_cursor = cur; }
  if (PRESSED(START)) {
    guide_open = 1;
    gv.sel = cur;
    gv.t0 = guide_slot(time(NULL));
    gv.top = cur > 2 ? cur - 2 : 0;                  /* a little context above */
    guide_scroll(&gv, chans.count);
  }
  if (FACE(PAD_Y)) {
    zoom = !zoom;
    player_set_zoom(pl, zoom);
    show_toast(zoom ? "ZOOM" : "FIT");
    logf_(RETRO_LOG_INFO, "aspect %s", zoom ? "zoom" : "fit");
  }
#undef FACE
#undef PAD_A
#undef PAD_B
#undef PAD_Y
#undef PRESSED
}

/* ---------- libretro API ---------- */

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb)
{
  env_cb = cb;
  struct retro_log_callback logging;
  if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) log_cb = logging.log;
  bool no_game = true;   /* launched from ReplayOS Extra without a playlist path */
  cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
  options_version = options_register(cb);
  options_defaults(&opts);
}
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
  memset(info, 0, sizeof(*info));
  info->library_name = "RetroTV";
  info->library_version = RETROTV_VERSION;
  info->valid_extensions = "m3u|m3u8|lr|txt";
  info->need_fullpath = true;
  info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
  memset(info, 0, sizeof(*info));
  info->geometry.base_width = PLAYER_W;
  info->geometry.base_height = PLAYER_H;
  info->geometry.max_width = PLAYER_W;
  info->geometry.max_height = PLAYER_H;
  info->geometry.aspect_ratio = 4.0f / 3.0f;
  info->timing.fps = fps_out;
  info->timing.sample_rate = PLAYER_RATE;
}

RETRO_API void retro_init(void) { frame_no = 0; t_clock = 0; fps_out = 60.0; }
RETRO_API void retro_deinit(void) {}
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }
RETRO_API void retro_reset(void) { tune(cur); }

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
  enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
  if (!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) { logf_(RETRO_LOG_ERROR, "XRGB8888 not supported"); return false; }

  char err[256];
  if (ffapi_load(err, sizeof(err))) { logf_(RETRO_LOG_ERROR, "FFmpeg: %s", err); return false; }
  apply_options();
  p_av_log_set_level(AV_LOG_FATAL);   /* joining live streams mid-GOP logs harmless h264 errors */
  p_avformat_network_init();

  const char *gp = (game && game->path && *game->path) ? game->path : NULL;
  char src[1024];
  help_reason[0] = '\0';
  logf_(RETRO_LOG_INFO, "RetroTV %s starting, content: %s", RETROTV_VERSION, gp ? gp : "(none)");
  if (resolve_playlist(gp, src, sizeof(src))) {
    snprintf(help_reason, sizeof(help_reason), "no playlist.txt found");
  } else if (load_playlist(src) == 0 && chans.count == 0) {
    snprintf(help_reason, sizeof(help_reason), "playlist has no channels: %.100s", src);
    ob_why = OB_NO_CHANNELS;
    if (!*ob_path) snprintf(ob_path, sizeof(ob_path), "%s", src);
  }
  pl = player_create();
  if (!pl) { m3u_free(&chans); return false; }
  cur = prev = 0;
  if (chans.count) {
    logf_(RETRO_LOG_INFO, "%zu channels loaded from %s", chans.count, src);
    last_init(src);
    guide_start(chans.epg_url);
    char *t = slurp(last_path, 4096);
    int idx = lastch_find(&chans, t);
    free(t);
    if (idx >= 0) logf_(RETRO_LOG_INFO, "resuming ch %s (last watched)", chans.ch[idx].number);
    char wp[1100];
    welcome_path(wp, sizeof(wp));
    char *seen = slurp(wp, 64);
    welcome_on = !seen;
    free(seen);
    saved_idx = idx;
    tune(idx >= 0 ? (size_t)idx : 0);
  } else {
    logf_(RETRO_LOG_WARN, "no channels: %s", help_reason);
  }
  return true;          /* show the help screen rather than a cryptic load error */
}

RETRO_API bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n) { (void)t; (void)i; (void)n; return false; }

RETRO_API void retro_unload_game(void)
{
  log_stats();
  guide_shutdown();
  if (pl) { player_destroy(pl); pl = NULL; }
  for (int i = 0; i < 200 && atomic_load(&saving); i++) {      /* let a pending write finish (max 2 s) */
    struct timespec ts = { 0, 10 * 1000000L };
    nanosleep(&ts, NULL);
  }
  m3u_free(&chans);
  last_path[0] = store_dir[0] = state_dir[0] = '\0';
  ob_why = OB_NO_FILE;
  ob_err[0] = ob_path[0] = '\0';
  testcard_on = welcome_on = welcome_save = 0;
  saved_idx = -1;
}

RETRO_API void retro_run(void)
{
  input_poll_cb();
  check_options();
  handle_input();

  if (testcard_on) {
    osd_surface ts = { fb, PLAYER_W, PLAYER_H, PLAYER_W };
    char clk[12];
    epg_clock(time(NULL), opts.clock24, 1, clk, sizeof(clk));
    testcard_draw(&ts, testcard_side(tc_t), clk);
    testcard_audio(abuf, aframes(), PLAYER_RATE, tc_t, &tc_phase);
    video_cb(fb, PLAYER_W, PLAYER_H, PLAYER_W * sizeof(uint32_t));
    audio_batch_cb(abuf, aframes());
    tc_t += 1.0 / fps_out;
    frame_no++;
    t_clock += 1.0 / fps_out;
    return;
  }
  if (!chans.count) {
    osd_surface hs = { fb, PLAYER_W, PLAYER_H, PLAYER_W };
    draw_help(&hs);
    memset(abuf, 0, sizeof(abuf));
    video_cb(fb, PLAYER_W, PLAYER_H, PLAYER_W * sizeof(uint32_t));
    audio_batch_cb(abuf, aframes());
    frame_no++;
    t_clock += 1.0 / fps_out;
    return;
  }

  player_state st = player_get_state(pl);
  player_audio(pl, abuf, aframes(), 1.0 / fps_out);
  if (opts.leveller) loud_process(&loud, abuf, aframes(), PLAYER_RATE);   /* even out channel loudness */
  st = player_get_state(pl);
  char why[160];
  int att = player_attempt(pl, why, sizeof(why));
  if (att > logged_attempt) {
    logf_(RETRO_LOG_WARN, "ch %s attempt %d failed (%s), retrying", chans.ch[cur].number, att - 1, why);
    logged_attempt = att;
  }
  if (st == PLAYER_ERROR && !logged_error) {
    logf_(RETRO_LOG_WARN, "ch %s NO SIGNAL after %.1fs: %s", chans.ch[cur].number, player_tuning_seconds(pl), player_error(pl));
    logged_error = 1;
  }
  if (st == PLAYER_PLAYING && !logged_playing) {
    player_info in;
    player_get_info(pl, &in);
    logf_(RETRO_LOG_INFO, "playing ch %s after %.2fs (%dx%d %s %.2ffps %s%s)", chans.ch[cur].number,
          player_tuning_seconds(pl), in.width, in.height, in.vcodec, in.fps, in.acodec, in.native ? ", native" : "");
    logged_playing = 1;
    played_at = t_now();
    banner_until = t_now() + opts.banner_secs;   /* show what came on */
    follow_refresh(in.fps);
  }

  int se = atomic_exchange(&save_errno, 0);
  if (se) logf_(RETRO_LOG_WARN, "could not save last channel to %s: %s", last_path, strerror(se));
  if (atomic_load(&guide_msg_ready)) {
    logf_(RETRO_LOG_INFO, "%s", guide_msg);
    atomic_store(&guide_msg_ready, 0);
  }
  if (welcome_save) {
    char wp[1100];
    welcome_path(wp, sizeof(wp));
    if (!save_text_async(wp, "seen=1\n")) welcome_save = 0;
  }
  if (st == PLAYER_PLAYING && saved_idx != (int)cur && t_now() - played_at >= REMEMBER_AFTER)
    last_save_async(cur);

  osd_surface s = { fb, PLAYER_W, PLAYER_H, PLAYER_W };
  int have = player_video(pl, fb);
  if (st == PLAYER_ERROR) {
    draw_no_signal(&s);
  } else if (st != PLAYER_PLAYING && !have) {
    if (opts.static_vhs) osd_static_vhs(&s, &noise_seed, (unsigned)frame_no);
    else osd_static(&s, &noise_seed);
    /* quiet hiss while tuning */
    for (size_t i = 0; i < aframes(); i++) {
      noise_seed ^= noise_seed << 13; noise_seed ^= noise_seed >> 17; noise_seed ^= noise_seed << 5;
      int16_t v = (int16_t)(((int)(noise_seed & 0x1fff) - 0x1000) * opts.static_vol / 2);   /* about -23 dBFS at Normal */
      abuf[2 * i] = abuf[2 * i + 1] = v;
    }
  }

  int show_banner = st != PLAYER_PLAYING || t_now() < banner_until;
  if (welcome_on) draw_welcome(&s);
  else if (guide_open) {
    pthread_mutex_lock(&guide_mu);
    guide_draw(&s, &chans, guide, &gv, cur, time(NULL));
    pthread_mutex_unlock(&guide_mu);
  } else if (list_open) draw_list(&s);
  else if (show_banner) draw_banner(&s, st);

  if (t_now() < toast_until) draw_toast(&s);

  video_cb(fb, PLAYER_W, PLAYER_H, PLAYER_W * sizeof(uint32_t));
  send_audio();
  frame_no++;
  t_clock += 1.0 / fps_out;
}

RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void *d, size_t s) { (void)d; (void)s; return false; }
RETRO_API bool retro_unserialize(const void *d, size_t s) { (void)d; (void)s; return false; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
