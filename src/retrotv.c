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
#include "gunzip.h"
#include "lastch.h"
#include "loud.h"
#include "onboard.h"
#include "options.h"
#include "plconf.h"
#include "testcard.h"
#include "theme.h"
#include "m3u.h"
#include "osd.h"
#include "player.h"
#include "remote.h"
#include "webui.h"

#define RETROTV_VERSION "0.2.7"
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
static uint32_t fb[PLAYER_W * PLAYER_H];   /* what we send: 640x480, or 640x240 in its top half */
static uint32_t vid[PLAYER_W * PLAYER_H];  /* 240p: the player's 640x480 picture before halving */
static int res240;                          /* output mode in use (opts.res240 is the wish) */
static int16_t abuf[AFRAMES_MAX * 2];
static size_t cur, prev;
static double banner_until;
static uint64_t frame_no;
static double fps_out = 60.0;      /* current refresh */
static double t_clock;             /* seconds of output since load */
static int refresh_refused;        /* frontend said no to SET_SYSTEM_AV_INFO */
static int zoom;                   /* TV aspect button: 0 FIT, 1 ZOOM */
static chanpref zprefs;            /* remembered Fit/Zoom per channel */
static char zoom_path[1100];
static int zoom_save;              /* zprefs changed: write when the saver is free */
static double stream_fps;          /* fps of the channel playing now (for the refresh option) */
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
static char epg_override[1024];        /* epg= from playlist.txt, else the playlist's x-tvg-url */
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
static int qm_open, qm_sel;          /* quick menu (X) */
enum { QM_PAUSE, QM_LIVE, QM_SLEEP, QM_AUDIO, QM_LEVEL, QM_ZOOM, QM_OUTPUT, QM_SCREEN2, QM_SWAP, QM_TESTCARD, QM_HELP, QM_CLOSE, QM_ROWS };
static int qm_rows[QM_ROWS], qm_n;     /* the rows shown: Output in single mode, Screen 2 / Swap in dual mode */
static int sleep_min;                /* sleep timer setting, minutes (0 = off) */
static double sleep_at;              /* t_now() when the set switches off (0 = no timer) */
static int sleep_warned;             /* "SLEEP 1" shown */
static int asleep;                   /* switched off by the sleep timer: black, silent, no stream */
#define SLEEP_FADE 5.0               /* seconds of fade before switching off */
#ifndef SLEEP_UNIT
#define SLEEP_UNIT 60.0              /* seconds per sleep-timer minute (tests build with 1) */
#endif
static const int SLEEP_STEPS[] = { 0, 15, 30, 60, 90 };
static guide_view gv;
static size_t list_cursor;
static uint16_t pad_prev;
static int logged_playing, logged_error, logged_attempt;

/* Two screens (dual mode): one 1280x480 picture that ReplayOS's "Dual
 * horizontal" splits across two CRTs. The globals above (pl, cur, prev,
 * zoom, banner, logs, last-channel file) are the screen with the focus: the
 * remote, menus, guide and sound act on it. `other` holds the second
 * screen; exchange() swaps them. The picture size never changes while
 * running (ReplayOS's dual output flashes on a mode change). */
typedef struct {
  player *pl;
  size_t cur, prev;
  int zoom, leveller;                  /* leveller: opts.leveller while it has the focus */
  double banner_until, played_at;
  int logged_playing, logged_error, logged_attempt;
  int saved_idx;
  char last_path[1100];
} screen_state;
static int dual;                       /* dual mode this session (opts.dual at load) */
static int screen2_on;                 /* screen 2 playing; off = standby card */
static int focus_side;                 /* physical screen with the focus: 0 = screen 1, 1 = screen 2 */
static int gside;                      /* physical screen whose state is in the globals right now */
static screen_state other;             /* the screen without the focus */
static uint32_t fb2[PLAYER_W * PLAYER_H];              /* the other screen's picture */
static uint32_t vid2[PLAYER_W * PLAYER_H];             /* 240p: its 640x480 picture before halving */
static uint32_t dualfb[2 * PLAYER_W * PLAYER_H];
static int16_t mutebuf[(PLAYER_RATE / 50) * 2];        /* the other screen's audio, read and dropped */
static double focus_flash_until;
static uint32_t noise2 = 777;
static char dual_path[1100];           /* remembers whether screen 2 is on */
static int dual_save;                  /* write it when the saver is free */
static int dual_changed_logged;
/* Two screens: ReplayOS's two outputs start with a random phase. Out of step,
 * it waits a whole extra refresh every frame and runs at exactly half rate
 * (measured on the Pi: 30 calls/s, every one 33 ms apart). Switching modes is
 * a new dice roll, and resyncing that way proved unreliable, so the output
 * size is fixed while two screens run; RetroTV only notes a half-rate start. */
static double pace_last, pace_win_start, pace_check_from, pace_noted;
static int pace_late, pace_frames;
static double mono_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
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

/* Remote control: the server thread resolves a request and leaves it here;
 * retro_run applies it. It reads chans (fixed while it runs) and the two
 * published values below, never the UI state. */
enum { RQ_TUNE = 1, RQ_UP, RQ_DOWN, RQ_LAST, RQ_FOCUS, RQ_PAUSE, RQ_LIVE };   /* RQ_PAUSE: channel field 0 play, 1 pause */
static atomic_int remote_req;          /* kind | screen << 3 (0 focused, 1, 2) | channel index << 5; 0 = nothing */
static atomic_int pub_cur = -1;        /* channel on the focused screen, for /status */
static atomic_int pub_dual, pub_s2on, pub_focus;       /* two screens: mode, screen 2 on, focused screen (0/1) */
static atomic_int pub_cur2 = -1, pub_state2;           /* the other screen's channel and state */
static atomic_int pub_paused, pub_behind, pub_paused2, pub_behind2;   /* pause: held?, seconds behind live */
enum { PS_TUNING, PS_PLAYING, PS_NO_SIGNAL, PS_OFF, PS_TESTCARD, PS_NO_CHANNELS };
static atomic_int pub_state = PS_NO_CHANNELS;
static int remote_mode;                /* what the server runs as: opts.remote values */
/* Settings from the phone page: retro_run publishes a snapshot for the server
 * thread, which leaves requested changes in rset for retro_run to apply. */
typedef struct {
  int leveller, zoom;
  int audio_cur, naudio;
  char audio_lang[8][8];
} picture_snap;
typedef struct {
  int sleep_min, sleep_left;           /* timer setting and minutes left (0 = off) */
  int res240, focus;                   /* focus: physical screen 0/1 */
  picture_snap pic[2];                 /* per physical screen; [1] only with two screens */
} remote_snap;
static pthread_mutex_t remote_mu = PTHREAD_MUTEX_INITIALIZER;
static remote_snap pubset;             /* under remote_mu */
static remote_cmd rset[3];             /* under remote_mu, by screen= (0 focused, 1, 2): kind RC_SET when something waits */

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

/* the surface to draw on: logical 640x480, half the rows in 240p */
static osd_surface screen(void) { return (osd_surface){ fb, PLAYER_W, PLAYER_H, PLAYER_W, res240 }; }
static void present(void)
{
  if (!dual) { video_cb(fb, PLAYER_W, res240 ? PLAYER_H / 2 : PLAYER_H, PLAYER_W * sizeof(uint32_t)); return; }
  const uint32_t *l = focus_side ? fb2 : fb, *r = focus_side ? fb : fb2;   /* screen 1 is the left half */
  int rows = res240 ? PLAYER_H / 2 : PLAYER_H;     /* 240p: both pictures are in their top halves */
  for (int y = 0; y < rows; y++) {
    memcpy(dualfb + (size_t)y * 2 * PLAYER_W, l + (size_t)y * PLAYER_W, PLAYER_W * sizeof(uint32_t));
    memcpy(dualfb + (size_t)y * 2 * PLAYER_W + PLAYER_W, r + (size_t)y * PLAYER_W, PLAYER_W * sizeof(uint32_t));
  }
  video_cb(dualfb, 2 * PLAYER_W, rows, 2 * PLAYER_W * sizeof(uint32_t));
}
/* "screen 2: " in log lines while two screens play */
static const char *stag(void) { return dual && screen2_on ? (gside ? "screen 2: " : "screen 1: ") : ""; }

/* 240p: average each pair of lines of the 640x480 picture into one */
static void halve_lines(const uint32_t *src, uint32_t *dst)
{
  for (int y = 0; y < PLAYER_H / 2; y++) {
    const uint32_t *a = src + (size_t)(2 * y) * PLAYER_W, *b = a + PLAYER_W;
    uint32_t *o = dst + (size_t)y * PLAYER_W;
    for (int x = 0; x < PLAYER_W; x++)
      o[x] = ((a[x] & 0xfefefe) >> 1) + ((b[x] & 0xfefefe) >> 1) + (a[x] & b[x] & 0x010101);
  }
}
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
  logf_(RETRO_LOG_INFO, "%stune ch %s \"%s\"", stag(), chans.ch[cur].number, chans.ch[cur].name);
  if (gside == focus_side) {                         /* the screen you hear */
    loud_reset(&loud);
    if (opts.blip) blip_left = BLIP_FRAMES;
  }
  int zr = opts.zoom_remember ? chanpref_get(&zprefs, &chans.ch[cur]) : -1;
  zoom = zr >= 0 ? zr : opts.zoom_default;
  player_set_zoom(pl, zoom);
  stream_fps = 0;
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

/* 3:05, or 1:02:03 */
static void mmss(double secs, char *out, size_t n)
{
  int t = secs > 0 ? (int)secs : 0;
  if (t >= 3600) snprintf(out, n, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
  else snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

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
  char now_t[48] = "", next_t[48] = "", t0[12] = "", t1[12] = "", t2[12] = "";
  double frac = -1;
  pthread_mutex_lock(&guide_mu);
  const epg_prog *pn, *px;
  player_shift sh;
  player_get_shift(pl, &sh);
  time_t tnow = time(NULL) - (time_t)sh.behind;     /* paused earlier: what's on at the moment you're watching */
  epg_now_next(guide_chan(guide, c), tnow, &pn, &px);
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
  if (sh.behind >= 1) {
    char b[16], t[32];
    mmss(sh.behind, b, sizeof(b));
    snprintf(t, sizeof(t), "-%s BEHIND LIVE", b);
    osd_text(s, bx + bw - osd_text_width(t, 1) - 14, by + 42, t, 1, TH->accent);
  }
  if (dual && screen2_on && gside == focus_side) {   /* the screen you hear: a little speaker */
    int sx = bx + bw - osd_text_width(clk, 2) - 14 - 34, sy = by + 18;
    osd_fill(s, sx, sy + 5, 5, 6, TH->accent, 255);
    osd_fill(s, sx + 5, sy + 3, 3, 10, TH->accent, 255);
    osd_fill(s, sx + 8, sy + 1, 3, 14, TH->accent, 255);
    osd_fill(s, sx + 14, sy + 4, 2, 8, TH->accent, 255);
    osd_fill(s, sx + 18, sy + 1, 2, 14, TH->accent, 255);
  }

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
  if (opts.hints) osd_text(s, nx, y + 16, "A INFO  B LAST  Y ZOOM  X MENU  START GUIDE", 1, TH->muted);
}

#define LIST_ROWS 12                   /* channels per page of the list (Left/Right page) */
static void draw_list(osd_surface *s)
{
  int rows = LIST_ROWS, x = 40, y = 40, w = PLAYER_W - 80, rh = 30;
  osd_fill(s, x, y, w, 36 + rows * rh, TH->bg, 230);
  osd_fill(s, x, y, w, 30, TH->accent, 255);
  osd_text(s, x + 12, y + 8, "CHANNEL LIST", 2, TH->on_accent);
  osd_text(s, x + w - 12 - osd_text_width("LEFT/RIGHT PAGE", 1), y + 12, "LEFT/RIGHT PAGE", 1, TH->on_accent);
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
    y += osd_line_h(s);
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
    { "Y", "zoom / fit picture" }, { "X", "menu: sleep, audio, leveller, test card" }, { "START", "TV guide" },
    { "SELECT", "channel list (left/right: a page)" },
    { "R2 / L2", "pause, play / back to live" },
    { "LEFT/RIGHT", "screen 1 / screen 2 (sound follows)" },
  };
  int n = dual ? 9 : 8, more = dual ? 22 : 0;        /* the last row only with two screens */
  for (int i = 0; i < n; i++) {
    osd_text_shadow(s, x + 24, y + 52 + i * 26, rows[i][0], 2, TH->accent);
    osd_text(s, x + 200, y + 56 + i * 26, rows[i][1], 1, TH->text);
  }
  osd_text(s, x + 24, y + 270 + more, "Settings: ReplayOS menu > System Settings", 1, TH->sub);
  osd_text_shadow(s, x + 24, y + 298 + more / 2 + 4, "Press A to start watching", 2, TH->text);
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

/* Download (or read, for a path) up to max bytes into a malloc'd buffer with a
 * NUL after the data; *len gets the size (data may contain NULs: gzip). */
static char *fetch_url(const char *url, size_t max, const AVIOInterruptCB *icb, size_t *len_out, char *err, size_t errlen)
{
  AVIOContext *io = NULL;
  AVDictionary *o = NULL;
  p_av_dict_set(&o, "rw_timeout", "10000000", 0);
  p_av_dict_set(&o, "user_agent", "RetroTV/" RETROTV_VERSION " (libretro)", 0);
  int rc = p_avio_open2(&io, url, AVIO_FLAG_READ, icb, &o);
  p_av_dict_free(&o);
  if (rc < 0) { char e[96]; p_av_strerror(rc, e, sizeof(e)); snprintf(err, errlen, "fetch failed: %s", e); return NULL; }
  size_t cap = 256 * 1024 < max ? 256 * 1024 : max, len = 0;
  char *buf = malloc(cap + 1);
  while (buf) {
    if (len == cap) {
      if (cap >= max) break;
      size_t nc = cap * 2 < max ? cap * 2 : max;
      char *nb = realloc(buf, nc + 1);
      if (!nb) { free(buf); buf = NULL; break; }
      buf = nb;
      cap = nc;
    }
    int n = p_avio_read(io, (unsigned char *)buf + len, (int)(cap - len));
    if (n <= 0) break;
    len += (size_t)n;
  }
  p_avio_closep(&io);
  if (!buf) { snprintf(err, errlen, "out of memory"); return NULL; }
  buf[len] = '\0';
  if (len_out) *len_out = len;
  return buf;
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
  plconf pc;
  if (game_path) {
    char *t = slurp(game_path, 4096);
    snprintf(buf, sizeof(buf), "%s", game_path);
    char *slash = strrchr(buf, '/');
    if (slash) *slash = '\0';
    plconf_parse(t, slash ? buf : ".", &pc);
    free(t);
    if (*pc.playlist) {
      snprintf(ob_path, sizeof(ob_path), "%s", game_path);    /* the file the link came from */
      snprintf(out, n, "%s", pc.playlist);
      snprintf(epg_override, sizeof(epg_override), "%s", pc.epg);
      return 0;
    }
  }
  static const char *roots[] = { "/media/nfs", "/media/usb", "/media/nvme", "/media/sd" };
  for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
    snprintf(buf, sizeof(buf), "%s/retrotv/playlist.txt", roots[i]);
    char *t = slurp(buf, 4096), dir[600];
    snprintf(dir, sizeof(dir), "%s/retrotv", roots[i]);
    plconf_parse(t, dir, &pc);
    if (t && !*pc.playlist && ob_why != OB_EMPTY_FILE) { ob_why = OB_EMPTY_FILE; snprintf(ob_path, sizeof(ob_path), "%s", buf); }
    free(t);
    if (*pc.playlist) {
      snprintf(store_dir, sizeof(store_dir), "%s", dir);
      snprintf(ob_path, sizeof(ob_path), "%s", buf);
      snprintf(out, n, "%s", pc.playlist);
      snprintf(epg_override, sizeof(epg_override), "%s", pc.epg);
      return 0;
    }
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
    char *text = fetch_url(src, 8u << 20, NULL, NULL, err, sizeof(err));
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
    size_t len = 0;
    char *xml = fetch_url(guide_url, 64u << 20, &icb, &len, err, sizeof(err));
    if (xml && gunzip_is_gzip(xml, len)) {           /* .xml.gz guides */
      char *un = gunzip(xml, len, 128u << 20, NULL, err, sizeof(err));
      free(xml);
      xml = un;
    }
    epg *g = NULL;
    if (xml) {
      time_t now = time(NULL);
      g = epg_parse(xml, now - GUIDE_BACK, now + GUIDE_AHEAD);
      if (g) g->match_numbers = !*epg_override;     /* another source numbers channels differently */
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
  snprintf(zoom_path, sizeof(zoom_path), "%s/retrotv-zoom-%08x.txt", dir, (unsigned)lastch_hash(src));
  char *zt = slurp(zoom_path, 1u << 20);
  chanpref_parse(&zprefs, zt);
  free(zt);
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

typedef struct { char path[1100]; char *text; } save_job;

static void *save_main(void *arg)
{
  save_job *j = arg;
  if (write_file(j->path, j->text)) atomic_store(&save_errno, errno ? errno : EIO);
  free(j->text);
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
  if (j && (j->text = strdup(text)) && !pthread_attr_init(&a)) {
    snprintf(j->path, sizeof(j->path), "%s", path);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    ok = !pthread_create(&t, &a, save_main, j);
    pthread_attr_destroy(&a);
  }
  if (!ok) { if (j) free(j->text); free(j); atomic_store(&saving, 0); return -1; }
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

static void follow_refresh(double fps);
static void exchange(void);
static void show_toast(const char *t);
static void remote_update(void);

/* the leveller's target for the screen in the globals */
static void loud_target(void) { loud.target_db = opts.leveller == 2 ? LOUD_TARGET_LOUD_DB : LOUD_TARGET_DB; }

static int saved_leveller = -1, saved_res240 = -1;  /* last values from System Settings */

/* Read the frontend's options. The X menu can change the leveller and the
 * output for the session; those stay until their own saved value changes. */
static void apply_options(void)
{
  int lev = opts.leveller, r240 = opts.res240;
  options_read(&opts, env_cb);
  if (saved_leveller >= 0 && opts.leveller == saved_leveller) opts.leveller = lev;
  else {                                             /* changed in System Settings: both screens */
    if (saved_leveller >= 0) other.leveller = opts.leveller;
    saved_leveller = opts.leveller;
  }
  if (saved_res240 >= 0 && opts.res240 == saved_res240) opts.res240 = r240;
  else saved_res240 = opts.res240;
  if (!pl) dual = opts.dual;                         /* dual mode is fixed for the session */
  if (!pl) res240 = opts.res240;                     /* in dual mode too: fixed for the session */                     /* at load the frontend asks for the size next */
  loud_target();
  theme_set(opts.theme);
  if (pl) player_set_margin(pl, opts.overscan);
  gv.clock24 = opts.clock24;
}

/* 480 <-> 240p while running (only from retro_run) */
static void set_output(int r240)
{
  if (dual) return;                                  /* fixed size with two screens: applied at start */
  if (r240 == res240) return;
  int was = res240;
  res240 = r240;
  struct retro_system_av_info av;
  retro_get_system_av_info(&av);
  if (env_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av)) {
    opts.res240 = r240;

    logf_(RETRO_LOG_INFO, "output %s", dual ? (res240 ? "1280x240 (240p, two screens)" : "1280x480 (two screens)") : res240 ? "640x240 (240p)" : "640x480");
  } else {
    res240 = opts.res240 = was;
    logf_(RETRO_LOG_WARN, "frontend refused the %s output", r240 ? "240p" : "480-line");
  }
}

/* the user changed something in the frontend's menu */
static void check_options(void)
{
  bool upd = false;
  if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &upd) && upd) {
    apply_options();
    if (dual && opts.res240 != res240 && !dual_changed_logged) {
      show_toast("RESTART RETROTV");
      logf_(RETRO_LOG_INFO, "output %s: applies when RetroTV restarts (two screens)", opts.res240 ? "240p" : "480");
      dual_changed_logged = 1;
    }
    if (opts.dual != dual && !dual_changed_logged) {
      show_toast("RESTART RETROTV");
      logf_(RETRO_LOG_INFO, "two screens %s: applies when RetroTV restarts", opts.dual ? "on" : "off");
      dual_changed_logged = 1;
    }
    follow_refresh(0);                               /* a refresh lock applies right away */
    set_output(opts.res240);
    remote_update();
    logf_(RETRO_LOG_INFO, "options: banner %.0fs, clock %s, leveller %s, static %d, hints %s, output %s", opts.banner_secs,
          opts.clock24 ? "24h" : "12h", opts.leveller == 2 ? "loud" : opts.leveller ? "normal" : "off",
          opts.static_vol, opts.hints ? "on" : "off", res240 ? "240p" : "480");
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

/* paused: like a VCR, top right, with how long it can stay paused */
static void draw_paused(osd_surface *s, const player_shift *sh)
{
  const char *big = "II PAUSED";
  int x = PLAYER_W - 48 - osd_text_width(big, 3);
  osd_text_shadow(s, x, 40, big, 3, OSD_RGB(80, 255, 80));
  char t[16], line[64];
  mmss(sh->paused_for, t, sizeof(t));
  if (sh->room >= 0) snprintf(line, sizeof(line), "%s  -  up to %d min more", t, (int)(sh->room / 60));
  else snprintf(line, sizeof(line), "%s", t);
  osd_text_shadow(s, PLAYER_W - 48 - osd_text_width(line, 1), 72, line, 1, OSD_RGB(80, 255, 80));
}

/* Ask the frontend for 50 Hz on 25/50 fps channels and 60 Hz otherwise. */
static void follow_refresh(double fps)
{
  if (dual) { if (fps > 0) stream_fps = fps; return; }   /* fixed 60 Hz: a mode change makes dual output flash */
  if (refresh_refused) return;
  if (fps > 0) stream_fps = fps;
  double want = opts.refresh ? opts.refresh                                   /* locked */
              : stream_fps <= 0 ? fps_out                                     /* nothing playing yet */
              : (fabs(stream_fps - 25.0) < 0.6 || fabs(stream_fps - 50.0) < 1.0) ? 50.0 : 60.0;
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
  logf_(RETRO_LOG_INFO, "refresh %.0f Hz -> %.0f Hz (%s, stream %.2f fps)", was, want, opts.refresh ? "locked" : "auto", stream_fps);
}

/* ---------- quick menu and sleep timer ---------- */

static void set_sleep(int minutes)
{
  sleep_min = minutes;
  sleep_at = minutes ? t_now() + minutes * SLEEP_UNIT : 0;
  sleep_warned = 0;
  char t[16];
  if (minutes) snprintf(t, sizeof(t), "SLEEP %d", minutes);
  else snprintf(t, sizeof(t), "SLEEP OFF");
  show_toast(t);
  logf_(RETRO_LOG_INFO, "sleep timer %d min", minutes);
}

static void step_sleep(int dir)
{
  int n = (int)(sizeof(SLEEP_STEPS) / sizeof(SLEEP_STEPS[0])), i = 0;
  while (i < n - 1 && SLEEP_STEPS[i] != sleep_min) i++;
  set_sleep(SLEEP_STEPS[(i + dir + n) % n]);
}

/* zoom for the screen in the globals (kept per channel when remembered) */
static void zoom_here(int z)
{
  zoom = z;
  player_set_zoom(pl, zoom);
  if (opts.zoom_remember && !chanpref_set(&zprefs, &chans.ch[cur], zoom)) zoom_save = 1;
}

static void set_zoom(int z)
{
  zoom_here(z);
  show_toast(zoom ? "ZOOM" : "FIT");
}

static void step_audio(int dir)
{
  player_info in;
  player_get_info(pl, &in);
  if (in.naudio < 2) return;
  int k = (in.audio_cur + dir + in.naudio) % in.naudio;
  char t[24];
  snprintf(t, sizeof(t), "AUDIO %d %s", k + 1, k < 8 ? in.audio_lang[k] : "");
  show_toast(t);
  logf_(RETRO_LOG_INFO, "audio track %d of %d", k + 1, in.naudio);
  player_open_track(pl, chans.ch[cur].url, k);       /* re-tune with that track */
}

static const char *const LEVELLER_NAMES[3] = { "Off", "Normal", "Loud" };

/* the volume leveller for now; System Settings holds the saved default */
static void set_leveller(int v)
{
  opts.leveller = v;
  loud_target();
  if (v) loud_reset(&loud);                          /* re-settle from unity, quickly */
  char t[24];
  snprintf(t, sizeof(t), "LEVELLER %s", v == 2 ? "LOUD" : v ? "NORMAL" : "OFF");
  show_toast(t);
  logf_(RETRO_LOG_INFO, "volume leveller %s (this session)", LEVELLER_NAMES[v]);
}

static void step_leveller(int dir)
{
  static const int order[3] = { 1, 2, 0 };            /* Normal -> Loud -> Off */
  int i = 0;
  while (i < 2 && order[i] != opts.leveller) i++;
  set_leveller(order[(i + dir + 3) % 3]);
}

/* pause (timeshift) the screen in the globals; on: 1 pause, 0 play */
static void set_pause(int on)
{
  player_shift sh;
  player_get_shift(pl, &sh);
  if (on == sh.paused) return;
  if (on && player_get_state(pl) != PLAYER_PLAYING) return;
  player_pause(pl, on);
  char b[16], t[24];
  mmss(sh.behind, b, sizeof(b));
  if (on) logf_(RETRO_LOG_INFO, "%spause ch %s", stag(), chans.ch[cur].number);
  else logf_(RETRO_LOG_INFO, "%splay ch %s, %s behind live", stag(), chans.ch[cur].number, b);
  snprintf(t, sizeof(t), "PLAY -%s", b);
  if (!on && gside == focus_side) show_toast(t);
}

/* back to live: a fresh tune drops what was kept */
static void go_live(void)
{
  player_shift sh;
  player_get_shift(pl, &sh);
  if (!sh.paused && sh.behind < 1) return;
  logf_(RETRO_LOG_INFO, "%sback to live", stag());
  tune(cur);
  if (gside == focus_side) show_toast("LIVE");
}

/* the memory for paused channels is full: play on from where it is */
static void shift_tick(void)
{
  player_shift sh;
  player_get_shift(pl, &sh);
  if (!sh.paused || !sh.full) return;
  logf_(RETRO_LOG_WARN, "%spause memory full after %.0f s: playing", stag(), sh.paused_for);
  player_pause(pl, 0);
  if (gside == focus_side) show_toast("BUFFER FULL");
}

static void qm_build(void)
{
  qm_n = 0;
  player_shift sh;
  player_get_shift(pl, &sh);
  for (int r = 0; r < QM_ROWS; r++) {
    if (r == QM_LIVE && !sh.paused && sh.behind < 1) continue;
    if (r == QM_OUTPUT && dual) continue;             /* two screens: System Settings, at start */
    if (r == QM_SCREEN2 && !dual) continue;
    if (r == QM_SWAP && !(dual && screen2_on)) continue;
    qm_rows[qm_n++] = r;
  }
  if (qm_sel >= qm_n) qm_sel = qm_n - 1;
}

static void draw_qmenu(osd_surface *s)
{
  qm_build();
  int x = 40, y = 92, w = 330, rh = 34, h = 62 + qm_n * rh;     /* below the OSD message */
  osd_fill(s, x, y, w, h, TH->bg, 235);
  osd_fill(s, x, y, w, 30, TH->accent, 255);
  osd_text(s, x + 12, y + 8, "MENU", 2, TH->on_accent);
  player_info in;
  player_get_info(pl, &in);
  for (int i = 0; i < qm_n; i++) {
    int r = qm_rows[i];
    char val[32] = "";
    int ry = y + 38 + i * rh, dim = 0;
    const char *label = "";
    player_shift sh;
    player_get_shift(pl, &sh);
    switch (r) {
    case QM_PAUSE: {
      char b[16];
      label = "Pause";
      mmss(sh.paused ? sh.paused_for : sh.behind, b, sizeof(b));
      if (sh.paused) snprintf(val, sizeof(val), "Paused %s", b);
      else if (sh.behind >= 1) snprintf(val, sizeof(val), "Playing -%s", b);
      else snprintf(val, sizeof(val), "Live");
      break;
    }
    case QM_LIVE: label = "Back to live"; break;
    case QM_SLEEP:
      label = "Sleep";
      if (sleep_min) snprintf(val, sizeof(val), "%d min", sleep_min); else snprintf(val, sizeof(val), "Off");
      break;
    case QM_AUDIO:
      label = "Audio";
      if (in.naudio > 1) snprintf(val, sizeof(val), "%d/%d %s", in.audio_cur + 1, in.naudio, in.audio_cur < 8 ? in.audio_lang[in.audio_cur] : "");
      else { snprintf(val, sizeof(val), "one track"); dim = 1; }
      break;
    case QM_LEVEL: label = "Leveller"; snprintf(val, sizeof(val), "%s", LEVELLER_NAMES[opts.leveller]); break;
    case QM_ZOOM: label = "Picture"; snprintf(val, sizeof(val), "%s", zoom ? "Zoom" : "Fit"); break;
    case QM_OUTPUT: label = "Output"; snprintf(val, sizeof(val), "%s", res240 ? "240p" : "480"); break;
    case QM_SCREEN2: label = "Screen 2"; snprintf(val, sizeof(val), "%s", screen2_on ? "On" : "Off"); break;
    case QM_SWAP: label = "Swap screens"; break;
    case QM_TESTCARD: label = "Test card"; break;
    case QM_HELP: label = "Show controls"; break;
    case QM_CLOSE: label = "Close"; break;
    }
    if (i == qm_sel) osd_fill(s, x + 4, ry - 4, w - 8, rh - 2, TH->cell_sel, 255);
    osd_text_shadow(s, x + 14, ry + 2, label, 2, dim ? TH->muted : TH->text);
    if (*val) {
      char v[40];
      snprintf(v, sizeof(v), i == qm_sel && !dim ? "< %s >" : "%s", val);
      osd_text(s, x + w - 14 - osd_text_width(v, 1), ry + 6, v, 1, dim ? TH->muted : TH->title);
    }
  }
  osd_text(s, x + 14, y + h - 16, "LEFT/RIGHT CHANGE  A SELECT  B CLOSE", 1, TH->muted);
}

/* sleep timer: warn at one minute, fade the last seconds, then switch off */
static void sleep_tick(void)
{
  if (!sleep_at || asleep) return;
  double left = sleep_at - t_now();
  if (left <= SLEEP_UNIT && !sleep_warned) { show_toast("SLEEP 1"); sleep_warned = 1; }
  if (left <= 0) {
    asleep = 1;
    sleep_at = 0;
    sleep_min = 0;
    qm_open = guide_open = list_open = 0;
    player_close(pl);
    if (dual && screen2_on) { exchange(); player_close(pl); exchange(); }
    logf_(RETRO_LOG_INFO, "sleep timer: switched off");
  }
}

/* 1 = full picture/sound, 0 = off: the fade before the sleep timer switches off */
static double sleep_level(void)
{
  if (!sleep_at) return 1.0;
  double left = sleep_at - t_now();
  return left >= SLEEP_FADE ? 1.0 : left <= 0 ? 0.0 : left / SLEEP_FADE;
}

/* ---------- two screens ---------- */

/* swap the globals with `other`; keep_paths leaves each last-channel file
 * with its physical screen (used by Swap screens) */
static void swap_state(int keep_paths)
{
  screen_state g = { pl, cur, prev, zoom, opts.leveller, banner_until, played_at, logged_playing, logged_error, logged_attempt,
                     saved_idx, "" };
  memcpy(g.last_path, last_path, sizeof(g.last_path));
  pl = other.pl; cur = other.cur; prev = other.prev; zoom = other.zoom; opts.leveller = other.leveller;
  banner_until = other.banner_until; played_at = other.played_at;
  logged_playing = other.logged_playing; logged_error = other.logged_error; logged_attempt = other.logged_attempt;
  saved_idx = other.saved_idx;
  memcpy(last_path, other.last_path, sizeof(last_path));
  other = g;
  if (keep_paths) {                                  /* the files and levellers stay with their screens */
    int lv = opts.leveller;
    opts.leveller = other.leveller;
    other.leveller = lv;
    char t[sizeof(last_path)];
    memcpy(t, last_path, sizeof(t));
    memcpy(last_path, other.last_path, sizeof(last_path));
    memcpy(other.last_path, t, sizeof(t));
  }
}
/* work on the other screen in place (pair every call) */
static void exchange(void) { swap_state(0); gside ^= 1; }

static void dual_note_save(void) { dual_save = 1; }

/* move the focus (remote, menus, guide, sound) to physical screen side */
static void set_focus(int side)
{
  if (!dual || side == focus_side || (side == 1 && !screen2_on)) return;
  qm_open = guide_open = list_open = 0;
  exchange();
  focus_side = gside;
  loud_target();                                     /* that screen's leveller */
  loud_reset(&loud);                                 /* the other channel's loudness */
  focus_flash_until = t_now() + 1.5;
  banner_until = t_now() + (opts.banner_secs > 0 ? opts.banner_secs : INFO_SECONDS);
  logf_(RETRO_LOG_INFO, "focus: screen %d (ch %s)", focus_side + 1, chans.count ? chans.ch[cur].number : "-");
}

/* screen 2 on: tune its last channel and give it the focus; off: standby */
static void set_screen2(int on)
{
  if (!dual || !chans.count || on == screen2_on) return;
  if (on) {
    screen2_on = 1;
    exchange();                                      /* screen 2's state into the globals... */
    focus_side = gside;                              /* ...and it keeps the focus */
    tune(cur);
    loud_target();
    loud_reset(&loud);
    focus_flash_until = t_now() + 1.5;
    logf_(RETRO_LOG_INFO, "screen 2 on (ch %s)", chans.ch[cur].number);
  } else {
    set_focus(0);
    exchange();
    log_stats();
    player_close(pl);
    exchange();
    screen2_on = 0;
    show_toast("SCREEN 2 OFF");
    logf_(RETRO_LOG_INFO, "screen 2 off");
  }
  qm_open = 0;
  dual_note_save();
}

/* Swap screens: the two channels change places; the focus (and sound) stays
 * on the same physical screen, so you now hear the channel that moved there */
static void swap_screens(void)
{
  if (!dual || !screen2_on) return;
  swap_state(1);
  saved_idx = other.saved_idx = -1;                  /* both files now hold the wrong channel */
  played_at = other.played_at = t_now() - REMEMBER_AFTER;
  loud_reset(&loud);
  banner_until = other.banner_until = t_now() + (opts.banner_secs > 0 ? opts.banner_secs : INFO_SECONDS);
  focus_flash_until = t_now() + 1.5;
  qm_open = 0;
  show_toast("SWAPPED");
  logf_(RETRO_LOG_INFO, "swap screens: screen 1 ch %s, screen 2 ch %s",
        chans.ch[focus_side ? other.cur : cur].number, chans.ch[focus_side ? cur : other.cur].number);
}

/* per-frame bookkeeping for the screen in the globals: logs, refresh, last channel */
static void screen_status(player_state st)
{
  char why[160];
  int att = player_attempt(pl, why, sizeof(why));
  if (att > logged_attempt) {
    logf_(RETRO_LOG_WARN, "%sch %s attempt %d failed (%s), retrying", stag(), chans.ch[cur].number, att - 1, why);
    logged_attempt = att;
  }
  if (st == PLAYER_ERROR && !logged_error) {
    logf_(RETRO_LOG_WARN, "%sch %s NO SIGNAL after %.1fs: %s", stag(), chans.ch[cur].number, player_tuning_seconds(pl), player_error(pl));
    logged_error = 1;
  }
  if (st == PLAYER_PLAYING && !logged_playing) {
    player_info in;
    player_get_info(pl, &in);
    logf_(RETRO_LOG_INFO, "%splaying ch %s after %.2fs (%dx%d %s %.2ffps %s%s)", stag(), chans.ch[cur].number,
          player_tuning_seconds(pl), in.width, in.height, in.vcodec, in.fps, in.acodec, in.native ? ", native" : "");
    logged_playing = 1;
    played_at = t_now();
    banner_until = t_now() + opts.banner_secs;   /* show what came on */
    if (gside == focus_side) follow_refresh(in.fps);
  }
  if (st == PLAYER_PLAYING && logged_playing && stream_fps <= 0 && gside == focus_side) {   /* frame rate measured after it started */
    player_info in;
    player_get_info(pl, &in);
    if (in.fps > 0) {
      logf_(RETRO_LOG_INFO, "%sch %s frame rate %.3f fps (%dx%d)", stag(), chans.ch[cur].number, in.fps, in.width, in.height);
      follow_refresh(in.fps);
      stream_fps = in.fps;                           /* once, even if the frontend refused a change */
    }
  }
  if (st == PLAYER_PLAYING && saved_idx != (int)cur && t_now() - played_at >= REMEMBER_AFTER)
    last_save_async(cur);
}

/* screen 2 switched off: a standby card over dim static. The card jumps to
 * a new place every STANDBY_MOVE seconds so it can't burn into the CRT. */
#define STANDBY_MOVE 8.0
static void draw_standby(osd_surface *s)
{
  osd_static(s, &noise2);
  osd_fill(s, 0, 0, PLAYER_W, PLAYER_H, OSD_RGB(0, 0, 0), 150);
  int w = 400, h = 120, margin = 24;
  uint32_t k = (uint32_t)(t_now() / STANDBY_MOVE) * 2654435761u;   /* a fixed spot per period */
  k ^= k >> 15;
  int x = margin + (int)(k % (uint32_t)(PLAYER_W - w - 2 * margin + 1));
  int y = margin + (int)((k >> 8) % (uint32_t)(PLAYER_H - h - 2 * margin + 1));
  osd_fill(s, x, y, w, h, TH->bg, 240);
  osd_fill(s, x, y, w, 3, TH->accent, 255);
  osd_text_shadow(s, x + (w - osd_text_width("SCREEN 2", 3)) / 2, y + 22, "SCREEN 2", 3, TH->title);
  osd_text_shadow(s, x + (w - osd_text_width("STANDBY", 2)) / 2, y + 56, "STANDBY", 2, TH->text);
  const char *hint = "X MENU > SCREEN 2 TO TURN ON";
  osd_text(s, x + (w - osd_text_width(hint, 1)) / 2, y + 92, hint, 1, TH->muted);
}

/* a gold frame and "SCREEN n" for a moment on the screen that just got the focus */
static void draw_focus_flash(osd_surface *s)
{
  uint32_t c = TH->accent;
  osd_fill(s, 0, 0, PLAYER_W, 6, c, 255);
  osd_fill(s, 0, PLAYER_H - 6, PLAYER_W, 6, c, 255);
  osd_fill(s, 0, 0, 6, PLAYER_H, c, 255);
  osd_fill(s, PLAYER_W - 6, 0, 6, PLAYER_H, c, 255);
  char t[16];
  snprintf(t, sizeof(t), "SCREEN %d", focus_side + 1);
  int w = osd_text_width(t, 3) + 32, x = PLAYER_W - w - 24;
  osd_fill(s, x, 24, w, 40, TH->bg, 235);
  osd_fill(s, x, 24, w, 3, c, 255);
  osd_text_shadow(s, x + 16, 34, t, 3, TH->title);
}

/* the screen without focus: read its audio (it is that stream's clock),
 * keep its logs and last channel, draw its picture into fb2 */
static void other_frame(void)
{
  if (!dual) return;
  osd_surface s2 = { fb2, PLAYER_W, PLAYER_H, PLAYER_W, res240 };
  if (!screen2_on || !chans.count || !other.pl) { draw_standby(&s2); return; }
  exchange();
  player_audio(pl, mutebuf, aframes(), 1.0 / fps_out);
  player_state st = player_get_state(pl);
  screen_status(st);
  int have = player_video(pl, res240 ? vid2 : fb2);
  if (res240 && have) halve_lines(vid2, fb2);
  if (st == PLAYER_ERROR) draw_no_signal(&s2);
  else if (st != PLAYER_PLAYING && !have) {
    if (opts.static_vhs) osd_static_vhs(&s2, &noise2, (unsigned)frame_no);
    else osd_static(&s2, &noise2);
  }
  if (st != PLAYER_PLAYING || t_now() < banner_until) draw_banner(&s2, st);
  shift_tick();
  player_shift sh;
  player_get_shift(pl, &sh);
  if (sh.paused) draw_paused(&s2, &sh);
  exchange();
}

/* ---------- remote control ---------- */

static const char REMOTE_HELP[] =
  "RetroTV remote control\n"
  "  /                     remote control page (open it in a phone's browser)\n"
  "  /tune?ch=36           tune channel 36 (its number in the playlist)\n"
  "  /tune?name=star+trek  tune by name (first match)\n"
  "  /up  /down  /last     channel up, down, previous\n"
  "  /pause  /play  /live  pause (up to ~15 min), play on, back to live\n"
  "  /status               what's on (JSON)\n"
  "  /channels             every channel with Now/Next, logos and posters (JSON)\n"
  "  /settings             sleep timer, audio track, leveller, picture, output (JSON)\n"
  "  /set?sleep=30&audio=2&leveller=loud&picture=zoom&output=240   change any of them\n"
  "  two screens: add screen=1 or screen=2 to /tune /up /down /last /pause /play /live;\n"
  "  /settings?screen=2 and /set?picture=zoom&leveller=loud&screen=2 read and change\n"
  "  that screen's picture, leveller and audio track without moving the focus;\n"
  "  /focus?screen=2 moves the focus (and sound); /set?screen2=on|off\n";

/* "now":{"title":..,"start":..,"stop":..},"next":{..} for one channel (guide_mu held) */
static void remote_now_next(remote_buf *b, const m3u_channel *ch, time_t t)
{
  const epg_prog *pn, *px;
  epg_now_next(guide_chan(guide, ch), t, &pn, &px);
  const epg_prog *ps[2] = { pn, px };
  for (int k = 0; k < 2; k++) {
    remote_puts(b, k ? ",\"next\":" : ",\"now\":");
    if (!ps[k]) { remote_puts(b, "null"); continue; }
    remote_puts(b, "{\"title\":");
    remote_json(b, ps[k]->title);
    remote_printf(b, ",\"start\":%lld,\"stop\":%lld,\"icon\":", (long long)ps[k]->start, (long long)ps[k]->stop);
    remote_json(b, ps[k]->icon);
    remote_puts(b, "}");
  }
}

/* the server thread: answer from chans and the published state only */
static int remote_answer(const remote_cmd *c, remote_buf *b, void *ctx)
{
  (void)ctx;
  static const char *const STATES[] = { "tuning", "playing", "no signal", "off", "test card", "no channels" };
  switch (c->kind) {
  case RC_PAGE: b->type = "text/html; charset=utf-8"; remote_puts(b, webui_page); return 200;
  case RC_HELP: remote_puts(b, REMOTE_HELP); return 200;
  case RC_NONE: remote_printf(b, "not found\n\n%s", REMOTE_HELP); return 404;
  case RC_BAD: remote_puts(b, "{\"ok\":false,\"error\":\"bad request: see /help\"}"); return 400;
  default: break;
  }
  if (!chans.count) { remote_puts(b, "{\"ok\":false,\"error\":\"no channels loaded\"}"); return 503; }
  if (c->screen == 2 && !(atomic_load(&pub_dual) && atomic_load(&pub_s2on))) {
    remote_puts(b, atomic_load(&pub_dual) ? "{\"ok\":false,\"error\":\"screen 2 is off: /set?screen2=on\"}"
                                          : "{\"ok\":false,\"error\":\"two screens are off (System Settings)\"}");
    return 409;
  }
  if (c->kind == RC_FOCUS) {
    atomic_store(&remote_req, RQ_FOCUS | c->screen << 3);
    remote_puts(b, "{\"ok\":true}");
    return 200;
  }
  if (c->kind == RC_PAUSE || c->kind == RC_PLAY || c->kind == RC_LIVE) {
    atomic_store(&remote_req, (c->kind == RC_LIVE ? RQ_LIVE : RQ_PAUSE) | c->screen << 3 | (c->kind == RC_PAUSE) << 5);
    remote_puts(b, "{\"ok\":true}");
    return 200;
  }
  int idx = atomic_load(&pub_cur);
  if (idx < 0 || (size_t)idx >= chans.count) idx = 0;
  time_t t = time(NULL);
  switch (c->kind) {
  case RC_CHANNELS:                                  /* no stream URLs: they can carry tokens */
    remote_puts(b, "{\"ok\":true,\"current\":");
    remote_json(b, chans.ch[idx].number);
    remote_printf(b, ",\"time\":%lld,\"channels\":[", (long long)t);
    pthread_mutex_lock(&guide_mu);
    for (size_t i = 0; i < chans.count; i++) {
      const m3u_channel *ch = &chans.ch[i];
      remote_puts(b, i ? ",{\"ch\":" : "{\"ch\":");
      remote_json(b, ch->number);
      remote_puts(b, ",\"name\":");
      remote_json(b, ch->name);
      remote_puts(b, ",\"group\":");
      remote_json(b, ch->group);
      remote_puts(b, ",\"logo\":");
      remote_json(b, ch->logo && *ch->logo ? ch->logo : NULL);
      remote_now_next(b, ch, t);
      remote_puts(b, "}");
    }
    pthread_mutex_unlock(&guide_mu);
    remote_puts(b, "]}");
    return 200;
  case RC_SETTINGS: {
    remote_snap v;
    pthread_mutex_lock(&remote_mu);
    v = pubset;
    pthread_mutex_unlock(&remote_mu);
    static const char *const LEV[3] = { "off", "normal", "loud" };
    const picture_snap *ps = &v.pic[c->screen ? c->screen - 1 : v.focus];
    remote_printf(b, "{\"ok\":true,\"sleep\":%d,\"sleep_left\":%d,\"sleep_steps\":[", v.sleep_min, v.sleep_left);
    for (size_t i = 0; i < sizeof(SLEEP_STEPS) / sizeof(SLEEP_STEPS[0]); i++) remote_printf(b, "%s%d", i ? "," : "", SLEEP_STEPS[i]);
    remote_printf(b, "],\"audio\":%d,\"audio_tracks\":[", ps->audio_cur + 1);
    for (int i = 0; i < ps->naudio && i < 8; i++) {
      if (i) remote_puts(b, ",");
      remote_json(b, ps->audio_lang[i]);
    }
    remote_printf(b, "],\"leveller\":\"%s\",\"picture\":\"%s\",\"output\":\"%s\",\"dual\":%s,\"screen2\":\"%s\",\"focus\":%d",
                  LEV[ps->leveller >= 0 && ps->leveller <= 2 ? ps->leveller : 1], ps->zoom ? "zoom" : "fit", v.res240 ? "240" : "480",
                  atomic_load(&pub_dual) ? "true" : "false", atomic_load(&pub_s2on) ? "on" : "off", v.focus + 1);
    if (c->screen) remote_printf(b, ",\"screen\":%d", c->screen);
    remote_puts(b, "}");
    return 200;
  }
  case RC_SET: {
    if (c->screen2 >= 0 && !atomic_load(&pub_dual)) {
      remote_puts(b, "{\"ok\":false,\"error\":\"two screens are off (System Settings)\"}");
      return 409;
    }
    int ok_sleep = c->sleep < 0;
    for (size_t i = 0; !ok_sleep && i < sizeof(SLEEP_STEPS) / sizeof(SLEEP_STEPS[0]); i++) ok_sleep = SLEEP_STEPS[i] == c->sleep;
    if (!ok_sleep) {
      remote_puts(b, "{\"ok\":false,\"error\":\"sleep must be one of");
      for (size_t i = 0; i < sizeof(SLEEP_STEPS) / sizeof(SLEEP_STEPS[0]); i++) remote_printf(b, " %d", SLEEP_STEPS[i]);
      remote_puts(b, "\"}");
      return 400;
    }
    pthread_mutex_lock(&remote_mu);
    int naudio = pubset.pic[c->screen ? c->screen - 1 : pubset.focus].naudio;
    if (c->audio > 0 && c->audio > naudio) {
      pthread_mutex_unlock(&remote_mu);
      remote_printf(b, "{\"ok\":false,\"error\":\"this channel has %d audio track%s\"}", naudio, naudio == 1 ? "" : "s");
      return 400;
    }
    remote_cmd *r = &rset[c->screen];
    if (r->kind != RC_SET) {                         /* merge with anything not applied yet */
      *r = *c;
    } else {
      if (c->sleep >= 0) r->sleep = c->sleep;
      if (c->audio > 0) r->audio = c->audio;
      if (c->leveller >= 0) r->leveller = c->leveller;
      if (c->picture >= 0) r->picture = c->picture;
      if (c->output >= 0) r->output = c->output;
      if (c->screen2 >= 0) r->screen2 = c->screen2;
    }
    pthread_mutex_unlock(&remote_mu);
    if (c->screen) remote_printf(b, "{\"ok\":true,\"screen\":%d}", c->screen);
    else remote_puts(b, "{\"ok\":true}");
    return 200;
  }
  case RC_TUNE:
    idx = remote_find(&chans, c->ch, c->name);
    if (idx < 0) {
      remote_puts(b, "{\"ok\":false,\"error\":\"no such channel\",\"asked\":");
      remote_json(b, *c->ch ? c->ch : c->name);
      remote_puts(b, "}");
      return 404;
    }
    atomic_store(&remote_req, RQ_TUNE | c->screen << 3 | idx << 5);
    remote_puts(b, "{\"ok\":true,\"ch\":");
    remote_json(b, chans.ch[idx].number);
    remote_puts(b, ",\"name\":");
    remote_json(b, chans.ch[idx].name);
    remote_puts(b, "}");
    return 200;
  case RC_UP: case RC_DOWN: case RC_LAST:
    atomic_store(&remote_req, (c->kind == RC_UP ? RQ_UP : c->kind == RC_DOWN ? RQ_DOWN : RQ_LAST) | c->screen << 3);
    remote_puts(b, "{\"ok\":true}");                 /* the channel is known once applied: see /status */
    return 200;
  default: {                                         /* RC_STATUS */
    const m3u_channel *ch = &chans.ch[idx];
    const epg_prog *pn, *px;
    int st = atomic_load(&pub_state);
    remote_puts(b, "{\"ok\":true,\"ch\":");
    remote_json(b, ch->number);
    remote_puts(b, ",\"name\":");
    remote_json(b, ch->name);
    remote_printf(b, ",\"state\":\"%s\",\"now\":", STATES[st >= 0 && st <= PS_NO_CHANNELS ? st : 0]);
    int behind = atomic_load(&pub_behind);
    pthread_mutex_lock(&guide_mu);
    epg_now_next(guide_chan(guide, ch), t - behind, &pn, &px);   /* paused earlier: what you're watching */
    remote_json(b, pn ? pn->title : NULL);
    remote_puts(b, ",\"next\":");
    remote_json(b, px ? px->title : NULL);
    pthread_mutex_unlock(&guide_mu);
    remote_printf(b, ",\"channels\":%zu,\"paused\":%s,\"behind\":%d", chans.count, atomic_load(&pub_paused) ? "true" : "false", behind);
    if (atomic_load(&pub_dual)) {
      int s2on = atomic_load(&pub_s2on), c2 = atomic_load(&pub_cur2), st2 = atomic_load(&pub_state2);
      remote_printf(b, ",\"dual\":true,\"focus\":%d,\"screen2\":{\"on\":%s", atomic_load(&pub_focus) + 1, s2on ? "true" : "false");
      if (s2on && c2 >= 0 && (size_t)c2 < chans.count) {   /* the screen without focus */
        remote_puts(b, ",\"other_ch\":");
        remote_json(b, chans.ch[c2].number);
        remote_puts(b, ",\"other_name\":");
        remote_json(b, chans.ch[c2].name);
        remote_printf(b, ",\"other_state\":\"%s\",\"other_paused\":%s,\"other_behind\":%d", STATES[st2 >= 0 && st2 <= PS_NO_CHANNELS ? st2 : 0],
                      atomic_load(&pub_paused2) ? "true" : "false", atomic_load(&pub_behind2));
      }
      remote_puts(b, "}");
    }
    remote_puts(b, "}");
    return 200;
  }
  }
}

/* start, stop or move the server to match the option (from retro_run / load) */
static void remote_update(void)
{
  if (opts.remote == remote_mode) return;
  remote_stop();
  remote_mode = 0;
  if (!opts.remote) { logf_(RETRO_LOG_INFO, "remote control off"); return; }
  const char *pe = getenv("RETROTV_REMOTE_PORT");   /* tests: run beside an installed RetroTV */
  int port = pe && atoi(pe) > 0 ? atoi(pe) : REMOTE_PORT;
  int e = remote_start(opts.remote == 2, port, remote_answer, NULL);
  if (e) {
    logf_(RETRO_LOG_WARN, "remote control: port %d: %s", port, strerror(e));
    remote_mode = opts.remote;                       /* don't retry every frame */
    return;
  }
  remote_mode = opts.remote;
  logf_(RETRO_LOG_INFO, "remote control on %s:%d", opts.remote == 2 ? "all interfaces" : "127.0.0.1", port);
}

/* the screen without the focus: its picture, leveller and audio track change
 * quietly (no toast on the screen you're watching), the focus and sound stay */
static void remote_set_away(const remote_cmd *c)
{
  exchange();
  if (c->leveller >= 0 && c->leveller != opts.leveller) {
    opts.leveller = c->leveller;                     /* used when it gets the focus */
    logf_(RETRO_LOG_INFO, "%svolume leveller %s (this session)", stag(), LEVELLER_NAMES[c->leveller]);
  }
  if (chans.count && !asleep && !testcard_on) {
    if (c->picture >= 0 && c->picture != zoom) zoom_here(c->picture);
    if (c->audio > 0) {
      player_info in;
      player_get_info(pl, &in);
      if (c->audio <= in.naudio && c->audio - 1 != in.audio_cur) player_open_track(pl, chans.ch[cur].url, c->audio - 1);
    }
  }
  exchange();
}

/* a request from the server thread: like a press on the remote */
static void remote_apply_set(remote_cmd c, int target)
{
  logf_(RETRO_LOG_INFO, "remote: settings sleep %d audio %d leveller %d picture %d output %d screen %d",
        c.sleep, c.audio, c.leveller, c.picture, c.output, target);
  if (c.sleep >= 0 && c.sleep != sleep_min && !asleep) set_sleep(c.sleep);
  if (c.output >= 0 && c.output != res240) { set_output(c.output); show_toast(res240 ? "240P" : "480"); }
  if (c.screen2 >= 0 && !asleep) set_screen2(c.screen2);
  int side = target ? target - 1 : focus_side;
  if (side != focus_side) {
    if (dual && screen2_on) remote_set_away(&c);
    return;
  }
  if (c.leveller >= 0 && c.leveller != opts.leveller) set_leveller(c.leveller);
  if (!chans.count || asleep || testcard_on) return;
  if (c.picture >= 0 && c.picture != zoom) set_zoom(c.picture);
  if (c.audio > 0) {
    player_info in;
    player_get_info(pl, &in);
    if (c.audio <= in.naudio && c.audio - 1 != in.audio_cur) step_audio(c.audio - 1 - in.audio_cur);
  }
}

static void remote_apply_settings(void)
{
  remote_cmd c[3];
  pthread_mutex_lock(&remote_mu);
  memcpy(c, rset, sizeof(c));
  for (int k = 0; k < 3; k++) rset[k].kind = RC_NONE;
  pthread_mutex_unlock(&remote_mu);
  for (int k = 0; k < 3; k++)
    if (c[k].kind == RC_SET) remote_apply_set(c[k], k);
}

/* tune on the screen in the globals (a remote press) */
static void remote_tune_here(int kind, size_t idx)
{
  if (kind == RQ_UP) idx = (cur + 1) % chans.count;
  else if (kind == RQ_DOWN) idx = cur ? cur - 1 : chans.count - 1;
  else if (kind == RQ_LAST) idx = prev;
  if (idx >= chans.count) return;
  if (idx == cur && !asleep && !testcard_on && player_get_state(pl) != PLAYER_ERROR) {
    banner_until = t_now() + (opts.banner_secs > 0 ? opts.banner_secs : INFO_SECONDS);   /* already on: just say so */
    return;
  }
  logf_(RETRO_LOG_INFO, "remote: %sch %s", stag(), chans.ch[idx].number);
  tune(idx);
}

static void remote_apply(void)
{
  remote_apply_settings();
  int r = atomic_exchange(&remote_req, 0);
  if (!r || !chans.count) return;
  int kind = r & 7, target = (r >> 3) & 3;
  size_t idx = (size_t)(r >> 5);
  if (kind == RQ_FOCUS) { if (!asleep) set_focus(target - 1); return; }
  if (kind == RQ_PAUSE || kind == RQ_LIVE) {         /* the picture on screen: no waking, no clearing */
    int side = target ? target - 1 : focus_side;
    if (asleep || testcard_on || (side != focus_side && !(dual && screen2_on))) return;
    int away = side != focus_side;
    if (away) exchange();
    if (kind == RQ_LIVE) go_live(); else set_pause(idx == 1);
    if (away) exchange();
    return;
  }
  int side = target ? target - 1 : focus_side;
  if (side != focus_side && !(dual && screen2_on)) return;
  /* wake the set and clear the screen, as if the viewer picked up the remote */
  int woke = asleep;
  asleep = testcard_on = welcome_on = 0;
  qm_open = guide_open = list_open = 0;
  int away = side != focus_side;
  if (away) exchange();
  remote_tune_here(kind, idx);
  if (away) exchange();
  if (woke) {                                        /* bring back the screen(s) not just tuned */
    if (away) tune(cur);
    if (dual && screen2_on) { exchange(); if (!away) tune(cur); exchange(); }
  }
}

static void publish_state(void)
{
  int st = PS_TUNING;
  if (asleep) st = PS_OFF;
  else if (testcard_on) st = PS_TESTCARD;
  else if (!chans.count) st = PS_NO_CHANNELS;
  else {
    player_state ps = player_get_state(pl);
    st = ps == PLAYER_PLAYING ? PS_PLAYING : ps == PLAYER_ERROR ? PS_NO_SIGNAL : PS_TUNING;
  }
  atomic_store(&pub_state, st);
  atomic_store(&pub_cur, chans.count ? (int)cur : -1);
  atomic_store(&pub_dual, dual);
  atomic_store(&pub_s2on, screen2_on);
  atomic_store(&pub_focus, focus_side);
  if (dual && screen2_on && other.pl && chans.count) {
    player_state ps = player_get_state(other.pl);
    atomic_store(&pub_state2, asleep ? PS_OFF : ps == PLAYER_PLAYING ? PS_PLAYING : ps == PLAYER_ERROR ? PS_NO_SIGNAL : PS_TUNING);
    atomic_store(&pub_cur2, (int)other.cur);
  } else atomic_store(&pub_cur2, -1);
  player_shift sh;
  player_get_shift(pl, &sh);
  atomic_store(&pub_paused, sh.paused);
  atomic_store(&pub_behind, (int)sh.behind);
  if (dual && screen2_on && other.pl) player_get_shift(other.pl, &sh);
  else memset(&sh, 0, sizeof(sh));
  atomic_store(&pub_paused2, sh.paused);
  atomic_store(&pub_behind2, (int)sh.behind);
  if (frame_no % 15) return;                         /* settings: 4 times a second is plenty */
  remote_snap v = { 0 };
  v.sleep_min = sleep_min;
  v.sleep_left = sleep_at ? (int)ceil((sleep_at - t_now()) / SLEEP_UNIT) : 0;
  v.res240 = res240;
  v.focus = focus_side;
  picture_snap *here = &v.pic[focus_side], *away = &v.pic[!focus_side];
  here->leveller = opts.leveller;
  here->zoom = zoom;
  away->leveller = other.leveller;
  away->zoom = other.zoom;
  int live = chans.count && !asleep && !testcard_on;
  for (int k = 0; k < 2; k++) {
    player *p = k ? (dual && screen2_on ? other.pl : NULL) : pl;
    picture_snap *ps = k ? away : here;
    if (!p || !live) continue;
    player_info in;
    player_get_info(p, &in);
    ps->audio_cur = in.audio_cur;
    ps->naudio = in.naudio;
    memcpy(ps->audio_lang, in.audio_lang, sizeof(ps->audio_lang));
  }
  pthread_mutex_lock(&remote_mu);
  pubset = v;
  pthread_mutex_unlock(&remote_mu);
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
    RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2,
  };
  /* any controller works the TV, like any remote in the house: players 1-4
   * (e.g. a second pad, or a virtual pad such as Zaparoo's on player 2) */
  for (unsigned port = 0; port < 4; port++)
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
      if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, ids[i])) m |= (uint16_t)(1u << ids[i]);
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
#define PAD_X Y
#define FACE(btn) PRESSED(btn)    /* expands PAD_x before PRESSED pastes it */
  if (asleep) {                                       /* any button switches the set back on */
    if (down) {
      asleep = 0;
      if (chans.count) tune(cur);
      if (dual && screen2_on && chans.count) { exchange(); tune(cur); exchange(); }
    }
    return;
  }
  if (testcard_on) {
    if (FACE(PAD_A) || FACE(PAD_B) || PRESSED(START)) { testcard_on = 0; if (chans.count) tune(cur); }
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
  if (qm_open) {
    qm_build();
    if (PRESSED(UP))   qm_sel = (qm_sel + qm_n - 1) % qm_n;
    if (PRESSED(DOWN)) qm_sel = (qm_sel + 1) % qm_n;
    int row = qm_rows[qm_sel];
    int dir = PRESSED(LEFT) ? -1 : PRESSED(RIGHT) ? 1 : FACE(PAD_A) ? 1 : 0;
    if (dir && row == QM_PAUSE) { player_shift sh; player_get_shift(pl, &sh); set_pause(!sh.paused); }
    if (FACE(PAD_A) && row == QM_LIVE) { go_live(); qm_open = 0; }
    if (dir && row == QM_SLEEP) step_sleep(dir);
    if (dir && row == QM_AUDIO) step_audio(dir);
    if (dir && row == QM_LEVEL) step_leveller(dir);
    if (dir && row == QM_ZOOM) set_zoom(!zoom);
    if (dir && row == QM_OUTPUT) { set_output(!res240); show_toast(res240 ? "240P" : "480"); }
    if (dir && row == QM_SCREEN2) { set_screen2(!screen2_on); return; }
    if (FACE(PAD_A) && row == QM_SWAP) { swap_screens(); return; }
    if (FACE(PAD_A) && row == QM_TESTCARD) { qm_open = 0; testcard_on = 1; tc_t = 0; player_close(pl); }
    if (FACE(PAD_A) && row == QM_HELP) { qm_open = 0; welcome_on = 1; }
    if ((FACE(PAD_A) && row == QM_CLOSE) || FACE(PAD_B) || FACE(PAD_X)) qm_open = 0;
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
    if (PRESSED(LEFT))  list_cursor = list_cursor > LIST_ROWS ? list_cursor - LIST_ROWS : 0;          /* a page up */
    if (PRESSED(RIGHT)) list_cursor = list_cursor + LIST_ROWS < chans.count ? list_cursor + LIST_ROWS : chans.count - 1;
    if (FACE(PAD_A))   { list_open = 0; tune(list_cursor); }
    if (FACE(PAD_B) || PRESSED(SELECT)) list_open = 0;
    return;
  }
  if (PRESSED(UP) || PRESSED(R))   tune((cur + 1) % chans.count);
  if (PRESSED(DOWN) || PRESSED(L)) tune(cur ? cur - 1 : chans.count - 1);
  if (FACE(PAD_B))     tune(prev);
  if (FACE(PAD_A))     banner_until = t_now() + (opts.banner_secs > 0 ? opts.banner_secs : INFO_SECONDS);
  if (PRESSED(SELECT)) { list_open = 1; list_cursor = cur; }
  if (dual && PRESSED(LEFT)) set_focus(0);
  if (dual && PRESSED(RIGHT)) { if (screen2_on) set_focus(1); else show_toast("SCREEN 2 OFF"); }
  if (FACE(PAD_X)) { qm_open = 1; qm_sel = 0; }
  if (PRESSED(START)) {
    guide_open = 1;
    gv.sel = cur;
    gv.t0 = guide_slot(time(NULL));
    gv.top = cur > 2 ? cur - 2 : 0;                  /* a little context above */
    guide_scroll(&gv, chans.count);
  }
  if (PRESSED(R2)) { player_shift sh; player_get_shift(pl, &sh); set_pause(!sh.paused); }
  if (PRESSED(L2)) go_live();
  if (FACE(PAD_Y)) {
    set_zoom(!zoom);
    logf_(RETRO_LOG_INFO, "aspect %s", zoom ? "zoom" : "fit");
  }
#undef FACE
#undef PAD_A
#undef PAD_B
#undef PAD_Y
#undef PAD_X
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
  info->geometry.base_width = dual ? 2 * PLAYER_W : PLAYER_W;
  info->geometry.base_height = res240 ? PLAYER_H / 2 : PLAYER_H;
  info->geometry.max_width = dual ? 2 * PLAYER_W : PLAYER_W;
  info->geometry.max_height = PLAYER_H;
  info->geometry.aspect_ratio = dual ? 8.0f / 3.0f : 4.0f / 3.0f;
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
  logf_(RETRO_LOG_INFO, "core options: %s", options_version == 2 ? "v2" : options_version == 1 ? "v0 variables" : "not supported by frontend");
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
  player_set_margin(pl, opts.overscan);
  cur = prev = 0;
  if (chans.count) {
    logf_(RETRO_LOG_INFO, "%zu channels loaded from %s", chans.count, src);
    last_init(src);
    if (*epg_override) logf_(RETRO_LOG_INFO, "guide from playlist.txt: %s", epg_override);
    guide_start(*epg_override ? epg_override : chans.epg_url);
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
    if (dual) {
      unsigned h = (unsigned)lastch_hash(src);
      snprintf(dual_path, sizeof(dual_path), "%s/retrotv-dual-%08x.txt", state_dir, h);
      memset(&other, 0, sizeof(other));
      snprintf(other.last_path, sizeof(other.last_path), "%s/retrotv-last2-%08x.txt", state_dir, h);
      char *t2 = slurp(other.last_path, 4096);
      int i2 = lastch_find(&chans, t2);
      free(t2);
      other.cur = other.prev = i2 >= 0 ? (size_t)i2 : (cur + 1) % chans.count;
      other.saved_idx = i2;
      other.zoom = opts.zoom_default;
      other.leveller = opts.leveller;
      other.pl = player_create();
      if (other.pl) player_set_margin(other.pl, opts.overscan);
      char *d = slurp(dual_path, 64);
      int was_on = d && strstr(d, "screen2=on");
      free(d);
      logf_(RETRO_LOG_INFO, "two screens: 1280x%d at 60 Hz, screen 2 %s (ch %s)", res240 ? 240 : 480, was_on ? "on" : "off", chans.ch[other.cur].number);
      pace_win_start = 0;
      pace_noted = 0;
      pace_check_from = mono_now() + 3.0;            /* check once start-up has settled */
      if (was_on && other.pl) {
        screen2_on = 1;
        exchange();                                  /* tune screen 2 in the background; focus stays on 1 */
        tune(cur);
        exchange();
      }
    }
  } else {
    logf_(RETRO_LOG_WARN, "no channels: %s", help_reason);
  }
  remote_update();
  return true;          /* show the help screen rather than a cryptic load error */
}

RETRO_API bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n) { (void)t; (void)i; (void)n; return false; }

RETRO_API void retro_unload_game(void)
{
  log_stats();
  remote_stop();                                     /* it reads chans */
  remote_mode = 0;
  atomic_store(&remote_req, 0);
  atomic_store(&pub_cur, -1);
  for (int k = 0; k < 3; k++) rset[k].kind = RC_NONE;   /* the server is stopped: no lock needed */
  guide_shutdown();
  if (pl) { player_destroy(pl); pl = NULL; }
  if (other.pl) { player_destroy(other.pl); other.pl = NULL; }
  dual = screen2_on = focus_side = gside = dual_save = dual_changed_logged = 0;
  dual_path[0] = other.last_path[0] = '\0';
  for (int i = 0; i < 200 && atomic_load(&saving); i++) {      /* let a pending write finish (max 2 s) */
    struct timespec ts = { 0, 10 * 1000000L };
    nanosleep(&ts, NULL);
  }
  m3u_free(&chans);
  last_path[0] = store_dir[0] = state_dir[0] = epg_override[0] = '\0';
  ob_why = OB_NO_FILE;
  ob_err[0] = ob_path[0] = '\0';
  testcard_on = welcome_on = welcome_save = zoom_save = 0;
  qm_open = asleep = sleep_min = 0;
  sleep_at = 0;
  chanpref_free(&zprefs);
  zoom_path[0] = '\0';
  saved_idx = -1;
}

/* two screens: note (once a minute at most) when ReplayOS runs at half rate */
static void pace_tick(void)
{
  if (!dual) return;
  double t = mono_now();
  if (pace_last > 0 && t >= pace_check_from) {
    if (pace_win_start == 0) { pace_win_start = t; pace_late = pace_frames = 0; }
    pace_frames++;
    if (t - pace_last > 1.5 / fps_out) pace_late++;
    if (t - pace_win_start >= 2.0) {
      int half = pace_frames <= (int)(fps_out * 2 * 0.6) && pace_late * 10 >= pace_frames * 8;
      if (half && t - pace_noted > 60.0) {
        logf_(RETRO_LOG_WARN, "two screens out of step: ReplayOS at half rate (%d frames in 2 s) - reopen RetroTV", pace_frames);
        pace_noted = t;
      }
      pace_win_start = 0;
    }
  }
  pace_last = t;
}

RETRO_API void retro_run(void)
{
  pace_tick();
  input_poll_cb();
  check_options();
  handle_input();
  remote_apply();
  sleep_tick();
  publish_state();

  if (asleep) {                                      /* switched off: black and silent */
    memset(fb, 0, sizeof(fb));
    memset(fb2, 0, sizeof(fb2));
    memset(abuf, 0, sizeof(abuf));
    present();
    audio_batch_cb(abuf, aframes());
    frame_no++;
    t_clock += 1.0 / fps_out;
    return;
  }
  if (testcard_on) {
    osd_surface ts = screen();
    char clk[12];
    epg_clock(time(NULL), opts.clock24, 1, clk, sizeof(clk));
    testcard_draw(&ts, testcard_side(tc_t), clk);
    testcard_audio(abuf, aframes(), PLAYER_RATE, tc_t, &tc_phase);
    other_frame();
    present();
    audio_batch_cb(abuf, aframes());
    tc_t += 1.0 / fps_out;
    frame_no++;
    t_clock += 1.0 / fps_out;
    return;
  }
  if (!chans.count) {
    osd_surface hs = screen();
    draw_help(&hs);
    memset(abuf, 0, sizeof(abuf));
    other_frame();
    present();
    audio_batch_cb(abuf, aframes());
    frame_no++;
    t_clock += 1.0 / fps_out;
    return;
  }

  player_state st = player_get_state(pl);
  player_audio(pl, abuf, aframes(), 1.0 / fps_out);
  if (opts.leveller) loud_process(&loud, abuf, aframes(), PLAYER_RATE);   /* even out channel loudness */
  shift_tick();
  st = player_get_state(pl);
  screen_status(st);
  other_frame();

  int se = atomic_exchange(&save_errno, 0);
  if (se) logf_(RETRO_LOG_WARN, "could not save last channel to %s: %s", last_path, strerror(se));
  if (atomic_load(&guide_msg_ready)) {
    logf_(RETRO_LOG_INFO, "%s", guide_msg);
    atomic_store(&guide_msg_ready, 0);
  }
  if (zoom_save) {
    char *zt = chanpref_format(&zprefs);
    if (zt && !save_text_async(zoom_path, zt)) zoom_save = 0;
    free(zt);
  }
  if (welcome_save) {
    char wp[1100];
    welcome_path(wp, sizeof(wp));
    if (!save_text_async(wp, "seen=1\n")) welcome_save = 0;
  }
  if (dual_save && !save_text_async(dual_path, screen2_on ? "screen2=on\n" : "screen2=off\n")) dual_save = 0;

  osd_surface s = screen();
  int have = player_video(pl, res240 ? vid : fb);
  if (res240 && have) halve_lines(vid, fb);
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
  else if (qm_open) draw_qmenu(&s);
  else if (guide_open) {
    pthread_mutex_lock(&guide_mu);
    guide_draw(&s, &chans, guide, &gv, cur, time(NULL));
    pthread_mutex_unlock(&guide_mu);
  } else if (list_open) draw_list(&s);
  else if (show_banner) draw_banner(&s, st);

  {
    player_shift sh;
    player_get_shift(pl, &sh);
    if (sh.paused && !welcome_on && !qm_open && !guide_open && !list_open) draw_paused(&s, &sh);
  }
  if (t_now() < toast_until) draw_toast(&s);
  if (dual && t_now() < focus_flash_until) draw_focus_flash(&s);

  double lvl = sleep_level();                        /* fade out before the sleep timer switches off */
  if (lvl < 1.0) {
    uint32_t k = (uint32_t)(lvl * 256);
    for (size_t i = 0; i < sizeof(fb) / sizeof(fb[0]); i++) {
      uint32_t c = fb[i];
      fb[i] = ((((c >> 16) & 0xff) * k >> 8) << 16) | ((((c >> 8) & 0xff) * k >> 8) << 8) | ((c & 0xff) * k >> 8);
      if (!dual) continue;
      c = fb2[i];
      fb2[i] = ((((c >> 16) & 0xff) * k >> 8) << 16) | ((((c >> 8) & 0xff) * k >> 8) << 8) | ((c & 0xff) * k >> 8);
    }
    for (size_t i = 0; i < aframes() * 2; i++) abuf[i] = (int16_t)(abuf[i] * lvl);
  }

  present();
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
