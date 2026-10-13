#include "player.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ffapi.h"

#define VQ 6                                  /* decoded video frames held */
#define AQ_FRAMES (PLAYER_RATE * 3)           /* 3 s of audio */
#define NANCHOR 32
#define PREBUFFER_AUDIO 0.30                  /* seconds before PLAYING */
#define BUFFER_TIMEOUT 4.0                    /* give up waiting for A+V sync */
#define OPEN_ATTEMPTS 3                       /* cold IPTV channels sometimes fail once */

typedef struct { uint32_t *px; double pts; } vslot;
typedef struct { uint64_t pos; double pts; } anchor;

struct player {
  pthread_t thread;
  int thread_live;
  atomic_int abort;
  char *url;

  pthread_mutex_t mu;
  pthread_cond_t cv;            /* producer waits for space */

  atomic_int state;
  atomic_int attempt;           /* 1-based connection attempt */
  char err[160];
  char attempt_err[160];        /* why the previous attempt failed */
  player_info info;
  double open_time;
  double play_start_mono;       /* for streams without audio */
  int has_audio, has_video;
  double first_vpts;

  /* video queue (ring) */
  vslot vq[VQ];
  int vhead, vcount;
  uint32_t *shown;              /* last picture handed out */
  int have_shown;

  /* audio ring */
  int16_t *aq;
  uint64_t awrite, aread;       /* total frames written / read */
  anchor anc[NANCHOR];
  int nanc;
  double clock;                 /* seconds, stream timebase */
  int clock_valid;
  double underrun;              /* seconds of continuous silence padding */
};

static double now_mono(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void set_error(player *p, const char *what, int code)
{
  char buf[96] = "";
  if (code) p_av_strerror(code, buf, sizeof(buf));
  pthread_mutex_lock(&p->mu);
  snprintf(p->err, sizeof(p->err), "%s%s%s", what, code ? ": " : "", buf);
  pthread_mutex_unlock(&p->mu);
  atomic_store(&p->state, PLAYER_ERROR);
}

static int interrupt_cb(void *opaque) { return atomic_load(&((player *)opaque)->abort); }

/* ---------- producer side (thread) ---------- */

static int wait_space(player *p, int need_video, size_t need_audio)
{
  pthread_mutex_lock(&p->mu);
  while (!atomic_load(&p->abort)) {
    int vok = !need_video || p->vcount < VQ;
    int aok = (p->awrite - p->aread) + need_audio <= AQ_FRAMES;
    if (vok && aok) break;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 50 * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&p->cv, &p->mu, &ts);
  }
  pthread_mutex_unlock(&p->mu);
  return atomic_load(&p->abort) ? -1 : 0;
}

/* Scale into a 4:3 PLAYER_W x PLAYER_H frame, keeping the display aspect ratio. */
static void fit_rect(const AVFrame *f, AVRational sar, int *ox, int *oy, int *ow, int *oh)
{
  double s = (sar.num > 0 && sar.den > 0) ? (double)sar.num / sar.den : 1.0;
  double dar = (double)f->width * s / (double)f->height;
  double out = (double)PLAYER_W / PLAYER_H;
  if (dar > out) { *ow = PLAYER_W; *oh = (int)lround(PLAYER_W / dar); }
  else           { *oh = PLAYER_H; *ow = (int)lround(PLAYER_H * dar); }
  *ow &= ~1; *oh &= ~1;
  if (*ow < 2) *ow = 2;
  if (*oh < 2) *oh = 2;
  *ox = (PLAYER_W - *ow) / 2;
  *oy = (PLAYER_H - *oh) / 2;
}

static void push_audio(player *p, const int16_t *s, int n, double pts)
{
  pthread_mutex_lock(&p->mu);
  uint64_t pos = p->awrite;
  /* new anchor at stream start or on a timestamp jump (channel splice, ad break) */
  int need = p->nanc == 0;
  if (!need && !isnan(pts)) {
    anchor *a = &p->anc[(p->nanc - 1) % NANCHOR];
    double predicted = a->pts + (double)(pos - a->pos) / PLAYER_RATE;
    if (fabs(predicted - pts) > 0.10) need = 1;
  }
  if (need && !isnan(pts)) {
    p->anc[p->nanc % NANCHOR] = (anchor){ pos, pts };
    p->nanc++;
  }
  for (int i = 0; i < n; i++) {
    size_t idx = (size_t)((p->awrite + (uint64_t)i) % AQ_FRAMES) * 2;
    p->aq[idx] = s[2 * i];
    p->aq[idx + 1] = s[2 * i + 1];
  }
  p->awrite += (uint64_t)n;
  pthread_mutex_unlock(&p->mu);
}

static void *thread_main(void *arg)
{
  player *p = arg;
  AVFormatContext *fmt = NULL;
  AVCodecContext *vdec = NULL, *adec = NULL;
  struct SwsContext *sws = NULL;
  SwrContext *swr = NULL;
  AVPacket *pkt = NULL;
  AVFrame *fr = NULL;
  AVDictionary *opts = NULL;
  int16_t *abuf = NULL;
  int abuf_cap = 0, vi = -1, ai = -1, rc;

  for (int attempt = 1;; attempt++) {
    const char *what;
    atomic_store(&p->attempt, attempt);
    atomic_store(&p->state, PLAYER_CONNECTING);
    fmt = p_avformat_alloc_context();
    if (!fmt) { set_error(p, "out of memory", 0); goto done; }
    fmt->interrupt_callback.callback = interrupt_cb;
    fmt->interrupt_callback.opaque = p;
    p_av_dict_free(&opts);                                   /* open_input consumes it */
    p_av_dict_set(&opts, "rw_timeout", "25000000", 0);       /* 25 s: cold IPTV channels start slowly */
    p_av_dict_set(&opts, "reconnect", "1", 0);
    p_av_dict_set(&opts, "reconnect_streamed", "1", 0);
    p_av_dict_set(&opts, "reconnect_delay_max", "4", 0);
    p_av_dict_set(&opts, "probesize", "1000000", 0);         /* fast tuning */
    p_av_dict_set(&opts, "analyzeduration", "1000000", 0);
    p_av_dict_set(&opts, "user_agent", "RetroTV/0.1 (libretro)", 0);

    rc = p_avformat_open_input(&fmt, p->url, NULL, &opts);
    if (rc >= 0) {
      rc = p_avformat_find_stream_info(fmt, NULL);
      if (rc >= 0) break;
      p_avformat_close_input(&fmt);
      what = "could not read stream";
    } else {
      fmt = NULL;                                             /* freed by open_input */
      what = "could not open stream";
    }
    if (atomic_load(&p->abort)) goto done;
    int fatal = rc == AVERROR_HTTP_NOT_FOUND || rc == AVERROR_HTTP_FORBIDDEN || rc == AVERROR_HTTP_UNAUTHORIZED;
    if (fatal || attempt >= OPEN_ATTEMPTS) { set_error(p, what, rc); goto done; }
    char buf[96] = "";
    p_av_strerror(rc, buf, sizeof(buf));
    pthread_mutex_lock(&p->mu);
    snprintf(p->attempt_err, sizeof(p->attempt_err), "%s: %s", what, buf);
    pthread_mutex_unlock(&p->mu);
    for (int i = 0; i < 30 && !atomic_load(&p->abort); i++) {   /* 1.5 s, interruptible */
      struct timespec ts = { 0, 50 * 1000000L };
      nanosleep(&ts, NULL);
    }
    if (atomic_load(&p->abort)) goto done;
  }

  vi = p_av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  ai = p_av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vi, NULL, 0);
  if (vi < 0 && ai < 0) { set_error(p, "no audio or video in stream", 0); goto done; }

  if (vi >= 0) {
    AVStream *st = fmt->streams[vi];
    const AVCodec *c = p_avcodec_find_decoder(st->codecpar->codec_id);
    vdec = c ? p_avcodec_alloc_context3(c) : NULL;
    if (!vdec || p_avcodec_parameters_to_context(vdec, st->codecpar) < 0) { set_error(p, "no video decoder", 0); goto done; }
    vdec->thread_count = 0;      /* auto: use all Pi 5 cores */
    if ((rc = p_avcodec_open2(vdec, c, NULL)) < 0) { set_error(p, "video decoder failed", rc); goto done; }
    pthread_mutex_lock(&p->mu);
    snprintf(p->info.vcodec, sizeof(p->info.vcodec), "%s", c->name);
    p->info.width = st->codecpar->width;
    p->info.height = st->codecpar->height;
    if (st->avg_frame_rate.den) p->info.fps = (double)st->avg_frame_rate.num / st->avg_frame_rate.den;
    pthread_mutex_unlock(&p->mu);
    p->has_video = 1;
  }
  if (ai >= 0) {
    AVStream *st = fmt->streams[ai];
    const AVCodec *c = p_avcodec_find_decoder(st->codecpar->codec_id);
    adec = c ? p_avcodec_alloc_context3(c) : NULL;
    if (!adec || p_avcodec_parameters_to_context(adec, st->codecpar) < 0) { set_error(p, "no audio decoder", 0); goto done; }
    if ((rc = p_avcodec_open2(adec, c, NULL)) < 0) { set_error(p, "audio decoder failed", rc); goto done; }
    pthread_mutex_lock(&p->mu);
    snprintf(p->info.acodec, sizeof(p->info.acodec), "%s", c->name);
    pthread_mutex_unlock(&p->mu);
    p->has_audio = 1;
  }

  pkt = p_av_packet_alloc();
  fr = p_av_frame_alloc();
  if (!pkt || !fr) { set_error(p, "out of memory", 0); goto done; }
  atomic_store(&p->state, PLAYER_BUFFERING);

  while (!atomic_load(&p->abort)) {
    rc = p_av_read_frame(fmt, pkt);
    if (rc < 0) {
      if (!atomic_load(&p->abort)) set_error(p, rc == AVERROR_EOF ? "stream ended" : "stream lost", rc == AVERROR_EOF ? 0 : rc);
      break;
    }
    AVCodecContext *dec = pkt->stream_index == vi ? vdec : pkt->stream_index == ai ? adec : NULL;
    if (!dec) { p_av_packet_unref(pkt); continue; }
    AVStream *st = fmt->streams[pkt->stream_index];
    rc = p_avcodec_send_packet(dec, pkt);
    p_av_packet_unref(pkt);
    if (rc < 0 && rc != AVERROR(EAGAIN)) continue;     /* corrupt packet: skip it */

    while (!atomic_load(&p->abort) && p_avcodec_receive_frame(dec, fr) == 0) {
      double pts = fr->best_effort_timestamp == AV_NOPTS_VALUE ? NAN
                   : (double)fr->best_effort_timestamp * st->time_base.num / st->time_base.den;
      if (dec == vdec) {
        if (wait_space(p, 1, 0)) break;
        int ox, oy, ow, oh;
        AVRational sar = fr->sample_aspect_ratio.num ? fr->sample_aspect_ratio : st->sample_aspect_ratio;
        fit_rect(fr, sar, &ox, &oy, &ow, &oh);
        sws = p_sws_getCachedContext(sws, fr->width, fr->height, (enum AVPixelFormat)fr->format,
                                     ow, oh, AV_PIX_FMT_BGR0, SWS_BILINEAR, NULL, NULL, NULL);
        if (sws) {
          pthread_mutex_lock(&p->mu);
          vslot *slot = &p->vq[(p->vhead + p->vcount) % VQ];
          pthread_mutex_unlock(&p->mu);
          memset(slot->px, 0, (size_t)PLAYER_W * PLAYER_H * 4);
          uint8_t *dst[4] = { (uint8_t *)(slot->px + (size_t)oy * PLAYER_W + ox), NULL, NULL, NULL };
          int dstride[4] = { PLAYER_W * 4, 0, 0, 0 };
          p_sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dst, dstride);
          pthread_mutex_lock(&p->mu);
          slot->pts = pts;
          p->vcount++;
          pthread_mutex_unlock(&p->mu);
        }
      } else {
        if (!swr) {
          AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
          if (p_swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_S16, PLAYER_RATE,
                                    &fr->ch_layout, (enum AVSampleFormat)fr->format, fr->sample_rate, 0, NULL) < 0
              || p_swr_init(swr) < 0) {
            set_error(p, "audio resampler failed", 0);
            goto done;
          }
        }
        int maxout = (int)((int64_t)fr->nb_samples * PLAYER_RATE / (fr->sample_rate ? fr->sample_rate : PLAYER_RATE)) + 256;
        if (maxout > abuf_cap) {
          int16_t *nb = realloc(abuf, (size_t)maxout * 2 * sizeof(int16_t));
          if (!nb) { set_error(p, "out of memory", 0); goto done; }
          abuf = nb; abuf_cap = maxout;
        }
        uint8_t *outp[1] = { (uint8_t *)abuf };
        int n = p_swr_convert(swr, outp, abuf_cap, (const uint8_t **)fr->extended_data, fr->nb_samples);
        if (n > 0) {
          if (wait_space(p, 0, (size_t)n)) break;
          push_audio(p, abuf, n, pts);
        }
      }
      p_av_frame_unref(fr);
    }
  }

done:
  if (fr) p_av_frame_free(&fr);
  if (pkt) p_av_packet_free(&pkt);
  if (swr) p_swr_free(&swr);
  if (sws) p_sws_freeContext(sws);
  if (vdec) p_avcodec_free_context(&vdec);
  if (adec) p_avcodec_free_context(&adec);
  if (fmt) p_avformat_close_input(&fmt);
  p_av_dict_free(&opts);
  free(abuf);
  return NULL;
}

/* ---------- consumer side (libretro thread) ---------- */

player *player_create(void)
{
  player *p = calloc(1, sizeof(*p));
  if (!p) return NULL;
  pthread_mutex_init(&p->mu, NULL);
  pthread_cond_init(&p->cv, NULL);
  p->aq = calloc((size_t)AQ_FRAMES * 2, sizeof(int16_t));
  p->shown = calloc((size_t)PLAYER_W * PLAYER_H, 4);
  for (int i = 0; i < VQ; i++) p->vq[i].px = calloc((size_t)PLAYER_W * PLAYER_H, 4);
  int ok = p->aq && p->shown;
  for (int i = 0; i < VQ; i++) ok = ok && p->vq[i].px;
  if (!ok) { player_destroy(p); return NULL; }
  return p;
}

void player_destroy(player *p)
{
  if (!p) return;
  player_close(p);
  for (int i = 0; i < VQ; i++) free(p->vq[i].px);
  free(p->aq);
  free(p->shown);
  free(p->url);
  pthread_cond_destroy(&p->cv);
  pthread_mutex_destroy(&p->mu);
  free(p);
}

static void reset_buffers(player *p)
{
  p->vhead = p->vcount = 0;
  p->awrite = p->aread = 0;
  p->nanc = 0;
  p->clock = 0;
  p->clock_valid = 0;
  p->underrun = 0;
  p->have_shown = 0;
  p->has_audio = p->has_video = 0;
  p->first_vpts = NAN;
  p->err[0] = '\0';
  p->attempt_err[0] = '\0';
  atomic_store(&p->attempt, 0);
  memset(&p->info, 0, sizeof(p->info));
}

int player_open(player *p, const char *url)
{
  player_close(p);
  free(p->url);
  p->url = strdup(url);
  if (!p->url) return -1;
  reset_buffers(p);
  atomic_store(&p->abort, 0);
  atomic_store(&p->state, PLAYER_CONNECTING);
  p->open_time = now_mono();
  if (pthread_create(&p->thread, NULL, thread_main, p)) {
    atomic_store(&p->state, PLAYER_ERROR);
    snprintf(p->err, sizeof(p->err), "could not start player thread");
    return -1;
  }
  p->thread_live = 1;
  return 0;
}

void player_close(player *p)
{
  if (!p->thread_live) { atomic_store(&p->state, PLAYER_IDLE); return; }
  atomic_store(&p->abort, 1);
  pthread_mutex_lock(&p->mu);
  pthread_cond_broadcast(&p->cv);
  pthread_mutex_unlock(&p->mu);
  pthread_join(p->thread, NULL);
  p->thread_live = 0;
  atomic_store(&p->state, PLAYER_IDLE);
}

player_state player_get_state(player *p) { return (player_state)atomic_load(&p->state); }
const char *player_error(player *p) { return p->err; }
double player_tuning_seconds(player *p) { return now_mono() - p->open_time; }

int player_attempt(player *p, char *why, size_t n)
{
  pthread_mutex_lock(&p->mu);
  if (why && n) snprintf(why, n, "%s", p->attempt_err);
  pthread_mutex_unlock(&p->mu);
  return atomic_load(&p->attempt);
}

void player_get_info(player *p, player_info *info)
{
  pthread_mutex_lock(&p->mu);
  *info = p->info;
  pthread_mutex_unlock(&p->mu);
}

static double pts_at(player *p, uint64_t pos)
{
  /* latest anchor at or before pos */
  int lo = p->nanc > NANCHOR ? p->nanc - NANCHOR : 0;
  for (int i = p->nanc - 1; i >= lo; i--) {
    anchor *a = &p->anc[i % NANCHOR];
    if (a->pos <= pos) return a->pts + (double)(pos - a->pos) / PLAYER_RATE;
  }
  return NAN;
}

void player_audio(player *p, int16_t *out, size_t frames, double frame_seconds)
{
  memset(out, 0, frames * 2 * sizeof(int16_t));
  int st = atomic_load(&p->state);
  if (st != PLAYER_BUFFERING && st != PLAYER_PLAYING) return;

  pthread_mutex_lock(&p->mu);
  uint64_t avail = p->awrite - p->aread;

  if (st == PLAYER_BUFFERING) {
    int audio_ready = !p->has_audio || avail >= (uint64_t)(PREBUFFER_AUDIO * PLAYER_RATE);
    int video_ready = !p->has_video || p->vcount > 0;
    int timed_out = now_mono() - p->open_time > BUFFER_TIMEOUT && (avail > 0 || p->vcount > 0);
    if ((audio_ready && video_ready) || timed_out) {
      atomic_store(&p->state, PLAYER_PLAYING);
      p->play_start_mono = now_mono();
      p->underrun = 0;
      st = PLAYER_PLAYING;
    }
  }

  if (st == PLAYER_PLAYING) {
    if (p->has_audio) {
      size_t n = avail < frames ? (size_t)avail : frames;
      for (size_t i = 0; i < n; i++) {
        size_t idx = (size_t)((p->aread + i) % AQ_FRAMES) * 2;
        out[2 * i] = p->aq[idx];
        out[2 * i + 1] = p->aq[idx + 1];
      }
      p->aread += n;
      double c = pts_at(p, p->aread);
      if (!isnan(c)) { p->clock = c; p->clock_valid = 1; }
      p->underrun = n < frames ? p->underrun + frame_seconds : 0;
      if (p->underrun > 1.5) atomic_store(&p->state, PLAYER_BUFFERING);   /* rebuffer */
    } else if (p->has_video && !isnan(p->first_vpts)) {
      p->clock = p->first_vpts + (now_mono() - p->play_start_mono);
      p->clock_valid = 1;
    }
  }
  pthread_cond_broadcast(&p->cv);
  pthread_mutex_unlock(&p->mu);
}

int player_video(player *p, uint32_t *dst)
{
  pthread_mutex_lock(&p->mu);
  int st = atomic_load(&p->state);
  int advanced = 0;
  if (st == PLAYER_PLAYING) {
    while (p->vcount > 0) {
      vslot *head = &p->vq[p->vhead];
      if (isnan(p->first_vpts) && !isnan(head->pts)) {
        p->first_vpts = head->pts;
        if (!p->has_audio) { p->clock = head->pts; p->clock_valid = 1; }
      }
      int due;
      if (!p->clock_valid || isnan(head->pts)) due = 1;
      else if (fabs(head->pts - p->clock) > 1.0) due = !advanced;   /* timestamp jump: resync, one per frame */
      else due = head->pts <= p->clock + 0.005;
      if (!due) break;
      /* keep the newest due frame */
      memcpy(p->shown, head->px, (size_t)PLAYER_W * PLAYER_H * 4);
      p->have_shown = 1;
      p->vhead = (p->vhead + 1) % VQ;
      p->vcount--;
      advanced = 1;
    }
  }
  int have = p->have_shown;
  if (have) memcpy(dst, p->shown, (size_t)PLAYER_W * PLAYER_H * 4);
  pthread_cond_broadcast(&p->cv);
  pthread_mutex_unlock(&p->mu);
  return have;
}
