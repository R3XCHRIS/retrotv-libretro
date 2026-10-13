/* harness.c - headless libretro frontend for testing RetroTV on the real Pi
 * without touching ReplayOS. Runs the core in real time, presses buttons on a
 * script, logs audio/motion stats each second and saves PNG screenshots.
 *
 *   harness <core.so> <playlist.m3u> <seconds> <outdir> ["up@8 shot@6 b@20 ..."]
 *
 * Actions: up down l r a b select start = press that button at T seconds;
 *          shot = save outdir/shot_<T>.png
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/libretro.h"

#define MAXACT 64
static struct { char what[16]; double at; int done; } act[MAXACT];
static int nact;

static unsigned fb_w, fb_h;
static uint32_t *frame, *last;
static double t_run;
static int held_button = -1;
static double held_until;

static double audio_sumsq; static size_t audio_n;
static double motion_sum; static int motion_frames;
static int frames_total;

static void hlog(enum retro_log_level level, const char *fmt, ...)
{
  (void)level;
  va_list ap;
  va_start(ap, fmt);
  printf("  [%6.2fs] ", t_run);
  vprintf(fmt, ap);
  va_end(ap);
  fflush(stdout);
}

static bool env(unsigned cmd, void *data)
{
  switch (cmd) {
  case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    ((struct retro_log_callback *)data)->log = hlog;
    return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
  case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    return true;
  default:
    return false;
  }
}

static void video(const void *data, unsigned w, unsigned h, size_t pitch)
{
  if (!data) return;
  if (w != fb_w || h != fb_h) {
    fb_w = w; fb_h = h;
    frame = realloc(frame, (size_t)w * h * 4);
    last = calloc((size_t)w * h, 4);
  }
  for (unsigned y = 0; y < h; y++)
    memcpy(frame + (size_t)y * w, (const uint8_t *)data + y * pitch, (size_t)w * 4);
  size_t diff = 0;
  for (size_t i = 0; i < (size_t)w * h; i += 7) diff += frame[i] != last[i];
  motion_sum += (double)diff / ((double)w * h / 7.0);
  motion_frames++;
  memcpy(last, frame, (size_t)w * h * 4);
  frames_total++;
}

static void audio_sample(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch(const int16_t *d, size_t frames)
{
  for (size_t i = 0; i < frames * 2; i++) audio_sumsq += (double)d[i] * d[i];
  audio_n += frames * 2;
  return frames;
}
static void input_poll(void) {}

static int thread_count(void)
{
  FILE *f = fopen("/proc/self/status", "r");
  char line[128];
  int n = -1;
  while (f && fgets(line, sizeof(line), f))
    if (!strncmp(line, "Threads:", 8)) { n = atoi(line + 8); break; }
  if (f) fclose(f);
  return n;
}

static int button_id(const char *w)
{
  static const struct { const char *n; int id; } m[] = {
    {"up", RETRO_DEVICE_ID_JOYPAD_UP}, {"down", RETRO_DEVICE_ID_JOYPAD_DOWN},
    {"l", RETRO_DEVICE_ID_JOYPAD_L}, {"r", RETRO_DEVICE_ID_JOYPAD_R},
    {"a", RETRO_DEVICE_ID_JOYPAD_A}, {"b", RETRO_DEVICE_ID_JOYPAD_B},
    {"select", RETRO_DEVICE_ID_JOYPAD_SELECT}, {"start", RETRO_DEVICE_ID_JOYPAD_START},
  };
  for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++) if (!strcmp(m[i].n, w)) return m[i].id;
  return -1;
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
  (void)index;
  if (port || device != RETRO_DEVICE_JOYPAD) return 0;
  return (int)id == held_button && t_run < held_until;
}

/* ---- minimal PNG writer (stored deflate, no zlib dependency) ---- */
static uint32_t crc_table[256];
static void crc_init(void)
{
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
}
static uint32_t crc(uint32_t c, const uint8_t *b, size_t n)
{
  c ^= 0xffffffffu;
  while (n--) c = crc_table[(c ^ *b++) & 0xff] ^ (c >> 8);
  return c ^ 0xffffffffu;
}
static void put32(FILE *f, uint32_t v) { uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v }; fwrite(b, 1, 4, f); }
static void chunk(FILE *f, const char *type, const uint8_t *d, size_t n)
{
  /* PNG CRC covers the chunk type and data together */
  uint8_t *tmp = malloc(4 + n);
  if (!tmp) return;
  memcpy(tmp, type, 4);
  if (n) memcpy(tmp + 4, d, n);
  put32(f, (uint32_t)n);
  fwrite(tmp, 1, 4 + n, f);
  put32(f, crc(0, tmp, 4 + n));
  free(tmp);
}
static int write_png(const char *path, const uint32_t *px, unsigned w, unsigned h)
{
  size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
  uint8_t *raw = malloc(raw_len);
  if (!raw) return -1;
  for (unsigned y = 0; y < h; y++) {
    uint8_t *r = raw + (size_t)y * (1 + (size_t)w * 3);
    r[0] = 0;
    for (unsigned x = 0; x < w; x++) {
      uint32_t p = px[(size_t)y * w + x];
      r[1 + x * 3] = p >> 16; r[2 + x * 3] = p >> 8; r[3 + x * 3] = p;
    }
  }
  size_t nblocks = (raw_len + 65534) / 65535;
  size_t z_len = 2 + raw_len + nblocks * 5 + 4;
  uint8_t *z = malloc(z_len), *o = z;
  *o++ = 0x78; *o++ = 0x01;
  uint32_t a = 1, b = 0;
  for (size_t off = 0; off < raw_len; off += 65535) {
    size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
    *o++ = off + n >= raw_len;
    *o++ = n & 0xff; *o++ = n >> 8; *o++ = ~n & 0xff; *o++ = (~n >> 8) & 0xff;
    memcpy(o, raw + off, n); o += n;
    for (size_t i = 0; i < n; i++) { a = (a + raw[off + i]) % 65521; b = (b + a) % 65521; }
  }
  uint32_t adler = (b << 16) | a;
  *o++ = adler >> 24; *o++ = adler >> 16; *o++ = adler >> 8; *o++ = adler;

  FILE *f = fopen(path, "wb");
  if (!f) { free(raw); free(z); return -1; }
  fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
  uint8_t ihdr[13] = { w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0 };
  chunk(f, "IHDR", ihdr, 13);
  chunk(f, "IDAT", z, (size_t)(o - z));
  chunk(f, "IEND", NULL, 0);
  fclose(f);
  free(raw); free(z);
  return 0;
}

#define SYM(name) do { *(void **)(&name) = dlsym(h, #name); if (!name) { fprintf(stderr, "missing %s\n", #name); return 2; } } while (0)

int main(int argc, char **argv)
{
  if (argc < 5) { fprintf(stderr, "usage: %s core.so playlist.m3u seconds outdir [script]\n", argv[0]); return 2; }
  const char *outdir = argv[4];
  double seconds = atof(argv[3]);
  if (argc > 5) {
    char *sc = strdup(argv[5]), *save = NULL;
    for (char *tok = strtok_r(sc, " ,", &save); tok && nact < MAXACT; tok = strtok_r(NULL, " ,", &save)) {
      char *at = strchr(tok, '@');
      if (!at) continue;
      *at = '\0';
      snprintf(act[nact].what, sizeof(act[nact].what), "%s", tok);
      act[nact].at = atof(at + 1);
      nact++;
    }
  }
  crc_init();
  setvbuf(stdout, NULL, _IOLBF, 0);      /* stdout is a pipe over ssh: flush each line */

  void *h = dlopen(argv[1], RTLD_NOW);
  if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
  void (*retro_set_environment)(retro_environment_t);
  void (*retro_set_video_refresh)(retro_video_refresh_t);
  void (*retro_set_audio_sample)(retro_audio_sample_t);
  void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
  void (*retro_set_input_poll)(retro_input_poll_t);
  void (*retro_set_input_state)(retro_input_state_t);
  void (*retro_init)(void);
  void (*retro_get_system_info)(struct retro_system_info *);
  void (*retro_get_system_av_info)(struct retro_system_av_info *);
  bool (*retro_load_game)(const struct retro_game_info *);
  void (*retro_run)(void);
  void (*retro_unload_game)(void);
  void (*retro_deinit)(void);
  SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample);
  SYM(retro_set_audio_sample_batch); SYM(retro_set_input_poll); SYM(retro_set_input_state);
  SYM(retro_init); SYM(retro_get_system_info); SYM(retro_get_system_av_info);
  SYM(retro_load_game); SYM(retro_run); SYM(retro_unload_game); SYM(retro_deinit);

  retro_set_environment(env);
  retro_set_video_refresh(video);
  retro_set_audio_sample(audio_sample);
  retro_set_audio_sample_batch(audio_batch);
  retro_set_input_poll(input_poll);
  retro_set_input_state(input_state);
  retro_init();

  struct retro_system_info si;
  retro_get_system_info(&si);
  printf("core: %s %s (extensions: %s)\n", si.library_name, si.library_version, si.valid_extensions);
  struct retro_game_info gi = { argv[2], NULL, 0, NULL };
  if (!retro_load_game(&gi)) { fprintf(stderr, "retro_load_game failed\n"); return 1; }
  struct retro_system_av_info av;
  retro_get_system_av_info(&av);
  printf("av: %ux%u @ %.2f fps, %.0f Hz\n", av.geometry.base_width, av.geometry.base_height, av.timing.fps, av.timing.sample_rate);

  double fps = av.timing.fps;
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  long nframes = (long)(seconds * fps);
  double late_max = 0;
  for (long i = 0; i < nframes; i++) {
    t_run = i / fps;
    for (int k = 0; k < nact; k++) {
      if (act[k].done || t_run < act[k].at) continue;
      act[k].done = 1;
      if (!strcmp(act[k].what, "shot")) continue;    /* taken after this frame */
      held_button = button_id(act[k].what);
      held_until = t_run + 0.1;
      printf("  [%6.2fs] press %s\n", t_run, act[k].what);
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    retro_run();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double took = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (took > late_max) late_max = took;

    for (int k = 0; k < nact; k++) {
      if (strcmp(act[k].what, "shot") || act[k].done != 1 || t_run < act[k].at) continue;
      act[k].done = 2;
      char path[512];
      snprintf(path, sizeof(path), "%s/shot_%05.1f.png", outdir, act[k].at);
      if (frame && !write_png(path, frame, fb_w, fb_h)) printf("  [%6.2fs] saved %s\n", t_run, path);
    }
    if ((i + 1) % (long)fps == 0) {
      double rms = audio_n ? sqrt(audio_sumsq / audio_n) : 0;
      printf("  t=%3lds audio_rms=%6.0f motion=%5.1f%% worst_run=%.1fms threads=%d\n",
             (i + 1) / (long)fps, rms, motion_frames ? 100 * motion_sum / motion_frames : 0, late_max * 1000,
             thread_count());
      audio_sumsq = 0; audio_n = 0; motion_sum = 0; motion_frames = 0; late_max = 0;
    }
    /* real-time pacing: live streams arrive at 1x */
    struct timespec due = start;
    double target = (i + 1) / fps;
    due.tv_sec += (time_t)target;
    due.tv_nsec += (long)((target - floor(target)) * 1e9);
    if (due.tv_nsec >= 1000000000L) { due.tv_sec++; due.tv_nsec -= 1000000000L; }
#ifdef TIMER_ABSTIME
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, NULL);
#else /* macOS: no clock_nanosleep; sleep the remainder (host `make check` only) */
    struct timespec now, rem;
    clock_gettime(CLOCK_MONOTONIC, &now);
    rem.tv_sec = due.tv_sec - now.tv_sec;
    rem.tv_nsec = due.tv_nsec - now.tv_nsec;
    if (rem.tv_nsec < 0) { rem.tv_sec--; rem.tv_nsec += 1000000000L; }
    if (rem.tv_sec >= 0) nanosleep(&rem, NULL);
#endif
  }
  retro_unload_game();
  retro_deinit();
  printf("done: %d frames\n", frames_total);
  return 0;
}
