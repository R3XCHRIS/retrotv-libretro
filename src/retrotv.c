/* retrotv.c - RetroTV libretro core: flip through IPTV channels from an M3U
 * like a cable box. Prototype for ReplayOS (Raspberry Pi 5). */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libretro.h"
#include "ffapi.h"
#include "m3u.h"
#include "osd.h"
#include "player.h"

#define RETROTV_VERSION "0.1.0"
#define FPS_INT 60
#define FPS ((double)FPS_INT)
#define AFRAMES ((size_t)(PLAYER_RATE / FPS_INT)) /* 800 stereo frames per run */
_Static_assert(PLAYER_RATE % FPS_INT == 0, "audio frames per video frame must be whole");
#define BANNER_SECONDS 4.0

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
static int16_t abuf[AFRAMES * 2];
static size_t cur, prev;
static double banner_until;
static uint64_t frame_no;
static uint32_t noise_seed = 12345;
static int list_open;
static size_t list_cursor;
static uint16_t pad_prev;
static int logged_playing, logged_error, logged_attempt;

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
}

static double t_now(void) { return (double)frame_no / FPS; }

static void tune(size_t idx)
{
  if (!chans.count) return;
  idx %= chans.count;
  if (idx != cur) prev = cur;
  cur = idx;
  banner_until = t_now() + BANNER_SECONDS;
  logged_playing = logged_error = 0;
  logged_attempt = 1;
  logf_(RETRO_LOG_INFO, "tune ch %s \"%s\"", chans.ch[cur].number, chans.ch[cur].name);
  player_open(pl, chans.ch[cur].url);
}

static void clip_copy(char *dst, size_t n, const char *src, size_t maxchars)
{
  char *a = m3u_ascii(src);
  snprintf(dst, n, "%.*s", (int)maxchars, a ? a : "");
  free(a);
}

/* ---------- drawing ---------- */

static void draw_banner(osd_surface *s, player_state st)
{
  const m3u_channel *c = &chans.ch[cur];
  int bx = 24, by = PLAYER_H - 132, bw = PLAYER_W - 48, bh = 104;
  osd_fill(s, bx, by, bw, bh, OSD_RGB(8, 16, 72), 255);   /* solid, like a real cable box */           /* cable-box navy */
  osd_fill(s, bx, by, bw, 3, OSD_RGB(240, 200, 40), 255);          /* gold rule */

  char num[16], name[40], group[48], line[96];
  clip_copy(num, sizeof(num), c->number, 6);
  clip_copy(name, sizeof(name), c->name, 26);
  clip_copy(group, sizeof(group), c->group ? c->group : "", 40);

  osd_text_shadow(s, bx + 14, by + 16, num, 4, OSD_RGB(255, 214, 64));
  int nx = bx + 14 + osd_text_width(num, 4) + 18;
  osd_text_shadow(s, nx, by + 18, name, 2, OSD_RGB(255, 255, 255));
  if (*group) osd_text(s, nx, by + 42, group, 1, OSD_RGB(120, 210, 255));

  time_t tt = time(NULL);
  struct tm tmv;
  localtime_r(&tt, &tmv);
  char clk[8];
  strftime(clk, sizeof(clk), "%H:%M", &tmv);
  osd_text_shadow(s, bx + bw - osd_text_width(clk, 2) - 14, by + 18, clk, 2, OSD_RGB(255, 255, 255));

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
  int maxc = (bx + bw - 14 - nx) / 8;                 /* keep inside the banner */
  if (maxc > 0 && (int)strlen(line) > maxc) line[maxc] = '\0';
  osd_text(s, nx, by + 66, line, 1, st == PLAYER_ERROR ? OSD_RGB(255, 120, 120) : OSD_RGB(200, 200, 200));
  osd_text(s, nx, by + 82, "UP/DN CH  B LAST  A INFO  SELECT LIST", 1, OSD_RGB(130, 130, 160));
}

static void draw_list(osd_surface *s)
{
  int rows = 12, x = 40, y = 40, w = PLAYER_W - 80, rh = 30;
  osd_fill(s, x, y, w, 36 + rows * rh, OSD_RGB(8, 16, 72), 230);
  osd_fill(s, x, y, w, 30, OSD_RGB(240, 200, 40), 255);
  osd_text(s, x + 12, y + 8, "CHANNEL LIST", 2, OSD_RGB(8, 16, 72));
  size_t first = list_cursor >= (size_t)rows / 2 ? list_cursor - (size_t)rows / 2 : 0;
  if (chans.count > (size_t)rows && first + (size_t)rows > chans.count) first = chans.count - (size_t)rows;
  for (int r = 0; r < rows && first + (size_t)r < chans.count; r++) {
    size_t i = first + (size_t)r;
    int ry = y + 36 + r * rh;
    if (i == list_cursor) osd_fill(s, x + 4, ry - 4, w - 8, rh - 2, OSD_RGB(40, 90, 200), 255);
    char num[16], name[40];
    clip_copy(num, sizeof(num), chans.ch[i].number, 6);
    clip_copy(name, sizeof(name), chans.ch[i].name, 30);
    osd_text(s, x + 14, ry + 2, num, 2, i == cur ? OSD_RGB(255, 214, 64) : OSD_RGB(160, 200, 255));
    osd_text(s, x + 120, ry + 2, name, 2, OSD_RGB(255, 255, 255));
  }
}

static void draw_no_signal(osd_surface *s)
{
  osd_fill(s, 0, 0, PLAYER_W, PLAYER_H, OSD_RGB(16, 32, 160), 255);   /* classic blue screen */
  const char *t = "NO SIGNAL";
  osd_text_shadow(s, (PLAYER_W - osd_text_width(t, 4)) / 2, 150, t, 4, OSD_RGB(255, 255, 255));
}

/* ---------- input ---------- */

static uint16_t read_pad(void)
{
  uint16_t m = 0;
  static const unsigned ids[] = {
    RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_A,
    RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_L,
    RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_START,
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
  if (!chans.count) return;
  if (list_open) {
    if (PRESSED(UP))   list_cursor = list_cursor ? list_cursor - 1 : chans.count - 1;
    if (PRESSED(DOWN)) list_cursor = (list_cursor + 1) % chans.count;
    if (PRESSED(A))    { list_open = 0; tune(list_cursor); }
    if (PRESSED(B) || PRESSED(SELECT)) list_open = 0;
    return;
  }
  if (PRESSED(UP) || PRESSED(R))   tune((cur + 1) % chans.count);
  if (PRESSED(DOWN) || PRESSED(L)) tune(cur ? cur - 1 : chans.count - 1);
  if (PRESSED(B))      tune(prev);
  if (PRESSED(A))      banner_until = t_now() + BANNER_SECONDS;
  if (PRESSED(SELECT)) { list_open = 1; list_cursor = cur; }
#undef PRESSED
}

/* ---------- libretro API ---------- */

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb)
{
  env_cb = cb;
  struct retro_log_callback logging;
  if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) log_cb = logging.log;
  bool no_game = false;
  cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
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
  info->valid_extensions = "m3u|m3u8";
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
  info->timing.fps = FPS;
  info->timing.sample_rate = PLAYER_RATE;
}

RETRO_API void retro_init(void) { frame_no = 0; }
RETRO_API void retro_deinit(void) {}
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }
RETRO_API void retro_reset(void) { tune(cur); }

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
  if (!game || !game->path) { logf_(RETRO_LOG_ERROR, "no playlist given"); return false; }
  enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
  if (!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) { logf_(RETRO_LOG_ERROR, "XRGB8888 not supported"); return false; }

  char err[256];
  if (ffapi_load(err, sizeof(err))) { logf_(RETRO_LOG_ERROR, "FFmpeg: %s", err); return false; }
  p_av_log_set_level(AV_LOG_ERROR);
  p_avformat_network_init();

  if (m3u_load(game->path, &chans) || chans.count == 0) {
    logf_(RETRO_LOG_ERROR, "no channels in %s", game->path);
    m3u_free(&chans);
    return false;
  }
  logf_(RETRO_LOG_INFO, "%zu channels loaded from %s", chans.count, game->path);
  pl = player_create();
  if (!pl) { m3u_free(&chans); return false; }
  cur = prev = 0;
  tune(0);
  return true;
}

RETRO_API bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n) { (void)t; (void)i; (void)n; return false; }

RETRO_API void retro_unload_game(void)
{
  if (pl) { player_destroy(pl); pl = NULL; }
  m3u_free(&chans);
}

RETRO_API void retro_run(void)
{
  input_poll_cb();
  handle_input();

  player_state st = player_get_state(pl);
  player_audio(pl, abuf, AFRAMES, 1.0 / FPS);
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
    logf_(RETRO_LOG_INFO, "playing ch %s after %.2fs", chans.ch[cur].number, player_tuning_seconds(pl));
    logged_playing = 1;
    banner_until = t_now() + BANNER_SECONDS;     /* show what came on */
  }

  osd_surface s = { fb, PLAYER_W, PLAYER_H, PLAYER_W };
  int have = player_video(pl, fb);
  if (st == PLAYER_ERROR) {
    draw_no_signal(&s);
  } else if (st != PLAYER_PLAYING && !have) {
    osd_static(&s, &noise_seed);
    /* quiet hiss while tuning */
    for (size_t i = 0; i < AFRAMES; i++) {
      noise_seed ^= noise_seed << 13; noise_seed ^= noise_seed >> 17; noise_seed ^= noise_seed << 5;
      int16_t v = (int16_t)((int)(noise_seed & 0x7ff) - 0x400);
      abuf[2 * i] = abuf[2 * i + 1] = v;
    }
  }

  int show_banner = st != PLAYER_PLAYING || t_now() < banner_until;
  if (list_open) draw_list(&s);
  else if (show_banner) draw_banner(&s, st);

  video_cb(fb, PLAYER_W, PLAYER_H, PLAYER_W * sizeof(uint32_t));
  audio_batch_cb(abuf, AFRAMES);
  frame_no++;
}

RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void *d, size_t s) { (void)d; (void)s; return false; }
RETRO_API bool retro_unserialize(const void *d, size_t s) { (void)d; (void)s; return false; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
