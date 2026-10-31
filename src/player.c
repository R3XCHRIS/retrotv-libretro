#define _GNU_SOURCE                   /* pthread_setname_np */
#include "player.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aspect.h"
#include "ffapi.h"
#include "vsmooth.h"

#define VQ 6                                  /* decoded video frames held */
#define AQ_FRAMES (PLAYER_RATE * 3)           /* 3 s of audio */
#define NANCHOR 32
#define PREBUFFER_AUDIO 0.30                  /* seconds before PLAYING */
#define BUFFER_TIMEOUT 4.0                    /* give up waiting for A+V sync */
#define OPEN_ATTEMPTS 3                       /* cold IPTV channels sometimes fail once */
/* Pause (timeshift): a reader thread keeps receiving while the picture is
 * held, so the packets wait in memory. One budget for every stream: 1 GiB is
 * ~15 minutes of a 10 Mbit/s channel. Never paused, a stream reads only a
 * little ahead and the decoder sets the pace, as before. */
#ifndef SHIFT_MAX
#define SHIFT_MAX ((int64_t)1 << 30)          /* tests build with a small one */
#endif
#define SHIFT_SLACK (SHIFT_MAX / 20)           /* "full" this early: input and output rates drift a little */
#define SHIFT_IDLE ((int64_t)8 << 20)

typedef struct { uint32_t *px; double pts; } vslot;
typedef struct pktnode { AVPacket *pkt; int64_t bytes; struct pktnode *next; } pktnode;

static atomic_llong shift_total;              /* bytes waiting in every stream's packet queue */
typedef struct { uint64_t pos; double pts; } anchor;

/* One tune: its own decode thread and buffers. The player swaps streams on a
 * channel change and lets a reaper thread close the old one (see below). */
typedef struct stream stream;

struct stream {
  pthread_t thread;
  int thread_live;
  atomic_int abort;
  char *url;
  int want_audio;               /* preferred audio track (0-based), -1 = FFmpeg's best */

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
  atomic_int *zoom;             /* the player's aspect button: 1 = crop to fill 4:3 */
  atomic_int *margin;           /* the player's overscan margin, percent per side */
  aspect_tracker bars;          /* decode thread only */
  long vframes;                 /* decode thread only */
  vsmooth vsm;                  /* decode thread only: evens out jittery timestamps */
  double gaps[VSMOOTH_GAPS];    /* decode thread only: frame gaps, to measure an unknown rate */
  int ngaps;
  double last_raw_vpts;
  uint32_t *shown;              /* last picture handed out */
  int have_shown;
  player_stats stats;

  /* received packets: the reader thread fills the queue, the decode thread empties it */
  AVFormatContext *fmt;         /* the reader's once it runs */
  int vi, ai;
  pthread_t reader;
  atomic_int rstop;             /* the decode thread is done: stop reading */
  pthread_mutex_t qmu;
  pthread_cond_t qcv;
  pktnode *qhead, *qtail;       /* under qmu */
  int64_t qbytes;
  int qend, qend_rc;            /* the reader stopped: end of stream or an error */
  atomic_llong rx_bytes;        /* received since the tune */

  /* pause: the consumer side stops; packets keep arriving */
  atomic_int paused;
  atomic_int shifted;           /* paused at least once: the reader keeps up with live */
  double pause_start, behind;   /* libretro thread */
  long long rx_at_pause;

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

static void set_error(stream *p, const char *what, int code)
{
  char buf[96] = "";
  if (code) p_av_strerror(code, buf, sizeof(buf));
  pthread_mutex_lock(&p->mu);
  snprintf(p->err, sizeof(p->err), "%s%s%s", what, code ? ": " : "", buf);
  pthread_mutex_unlock(&p->mu);
  atomic_store(&p->state, PLAYER_ERROR);
}

static int interrupt_cb(void *opaque)
{
  stream *p = opaque;
  return atomic_load(&p->abort) || atomic_load(&p->rstop);
}

static void timed_wait(pthread_cond_t *cv, pthread_mutex_t *mu, long ms)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += ms * 1000000L;
  if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
  pthread_cond_timedwait(cv, mu, &ts);
}

/* ---------- received packets (timeshift queue) ---------- */

static int queue_room(stream *p)
{
  if (atomic_load(&shift_total) >= SHIFT_MAX) return 0;
  return atomic_load(&p->shifted) || p->qbytes < SHIFT_IDLE;
}

/* the reader: receive packets as they come, into the queue */
static void *reader_main(void *arg)
{
  stream *p = arg;
#ifdef __linux__
  pthread_setname_np(pthread_self(), "retrotv-rx");
#endif
  AVPacket *pkt = p_av_packet_alloc();
  int rc = pkt ? 0 : AVERROR(ENOMEM);
  while (pkt && !atomic_load(&p->abort) && !atomic_load(&p->rstop)) {
    pthread_mutex_lock(&p->qmu);
    int room = queue_room(p);
    if (!room) timed_wait(&p->qcv, &p->qmu, 50);
    pthread_mutex_unlock(&p->qmu);
    if (!room) continue;
    rc = p_av_read_frame(p->fmt, pkt);
    if (rc < 0) break;
    if (pkt->stream_index != p->vi && pkt->stream_index != p->ai) { p_av_packet_unref(pkt); continue; }
    pktnode *n = malloc(sizeof(*n));
    AVPacket *q = p_av_packet_alloc();
    if (!n || !q) { free(n); p_av_packet_free(&q); p_av_packet_unref(pkt); rc = AVERROR(ENOMEM); break; }
    p_av_packet_move_ref(q, pkt);
    *n = (pktnode){ q, (int64_t)q->size + (int64_t)(sizeof(pktnode) + sizeof(AVPacket) + 64), NULL };
    atomic_fetch_add(&shift_total, n->bytes);
    atomic_fetch_add(&p->rx_bytes, n->bytes);
    pthread_mutex_lock(&p->qmu);
    if (p->qtail) p->qtail->next = n; else p->qhead = n;
    p->qtail = n;
    p->qbytes += n->bytes;
    pthread_cond_broadcast(&p->qcv);
    pthread_mutex_unlock(&p->qmu);
  }
  p_av_packet_free(&pkt);
  pthread_mutex_lock(&p->qmu);
  p->qend = 1;
  p->qend_rc = rc;
  pthread_cond_broadcast(&p->qcv);
  pthread_mutex_unlock(&p->qmu);
  return NULL;
}

/* the next packet into pkt: 0, 1 when the reader stopped (its code in *end_rc), -1 aborted */
static int queue_pop(stream *p, AVPacket *pkt, int *end_rc)
{
  pthread_mutex_lock(&p->qmu);
  while (!p->qhead) {
    if (atomic_load(&p->abort)) { pthread_mutex_unlock(&p->qmu); return -1; }
    if (p->qend) { *end_rc = p->qend_rc; pthread_mutex_unlock(&p->qmu); return 1; }
    timed_wait(&p->qcv, &p->qmu, 50);
  }
  pktnode *n = p->qhead;
  p->qhead = n->next;
  if (!p->qhead) p->qtail = NULL;
  p->qbytes -= n->bytes;
  pthread_cond_broadcast(&p->qcv);
  pthread_mutex_unlock(&p->qmu);
  atomic_fetch_sub(&shift_total, n->bytes);
  p_av_packet_move_ref(pkt, n->pkt);
  p_av_packet_free(&n->pkt);
  free(n);
  return atomic_load(&p->abort) ? -1 : 0;
}

static void queue_clear(stream *p)
{
  pthread_mutex_lock(&p->qmu);
  pktnode *n = p->qhead;
  p->qhead = p->qtail = NULL;
  p->qbytes = 0;
  pthread_mutex_unlock(&p->qmu);
  while (n) {
    pktnode *next = n->next;
    atomic_fetch_sub(&shift_total, n->bytes);
    p_av_packet_free(&n->pkt);
    free(n);
    n = next;
  }
}

/* ---------- producer side (thread) ---------- */

static int wait_space(stream *p, int need_video, size_t need_audio)
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

/* Scale into the 4:3 PLAYER_W x PLAYER_H frame, inside an overscan margin of
 * margin percent on every side, keeping the display aspect ratio. */
static void fit_rect(const AVFrame *f, AVRational sar, int margin, int *ox, int *oy, int *ow, int *oh)
{
  int mx = (PLAYER_W * margin / 100) & ~1, my = (PLAYER_H * margin / 100) & ~1;
  int aw = PLAYER_W - 2 * mx, ah = PLAYER_H - 2 * my;
  double s = (sar.num > 0 && sar.den > 0) ? (double)sar.num / sar.den : 1.0;
  double dar = (double)f->width * s / (double)f->height;
  double out = (double)aw / ah;
  if (dar > out) { *ow = aw; *oh = (int)lround(aw / dar); }
  else           { *oh = ah; *ow = (int)lround(ah * dar); }
  *ow &= ~1; *oh &= ~1;
  if (*ow < 2) *ow = 2;
  if (*oh < 2) *oh = 2;
  *ox = mx + (aw - *ow) / 2;
  *oy = my + (ah - *oh) / 2;
}

/* 8-bit planar luma in plane 0 (yuv420p, nv12, ...): what aspect_bars reads. */
static int luma8(const AVFrame *f)
{
  const AVPixFmtDescriptor *d = p_av_pix_fmt_desc_get((enum AVPixelFormat)f->format);
  return d && !(d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL))
         && d->nb_components >= 3 && d->comp[0].plane == 0 && d->comp[0].depth == 8 && d->comp[0].step == 1;
}

/* Look for letterbox/pillarbox bars on every 3rd frame (cheap: sparse rows/columns). */
static void track_bars(stream *p, const AVFrame *f)
{
  int t, l;
  if (p->vframes++ % 3 || !luma8(f)) return;
  if (!aspect_bars(f->data[0], f->linesize[0], f->width, f->height, &t, &l)) return;
  aspect_track(&p->bars, t, l);
  pthread_mutex_lock(&p->mu);
  p->info.bars_top = p->bars.top;
  p->info.bars_left = p->bars.left;
  pthread_mutex_unlock(&p->mu);
}

/* ZOOM: crop the frame to the 4:3 centre of the picture inside the bars. */
static void zoom_crop(stream *p, AVFrame *f, AVRational sar)
{
  int x, y, cw, ch;
  double s = (sar.num > 0 && sar.den > 0) ? (double)sar.num / sar.den : 1.0;
  aspect_zoom_rect(f->width, f->height, s, p->bars.top, p->bars.left, &x, &y, &cw, &ch);
  if (cw >= f->width && ch >= f->height) return;
  f->crop_left = (size_t)x;
  f->crop_top = (size_t)y;
  f->crop_right = (size_t)(f->width - x - cw);
  f->crop_bottom = (size_t)(f->height - y - ch);
  p_av_frame_apply_cropping(f, AV_FRAME_CROP_UNALIGNED);
}

static void push_audio(stream *p, const int16_t *s, int n, double pts)
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
  stream *p = arg;
#ifdef __linux__
  pthread_setname_np(pthread_self(), "retrotv-dec");   /* visible in top -H / ps -L */
#endif
  AVFormatContext *fmt = NULL;
  AVCodecContext *vdec = NULL, *adec = NULL;
  struct SwsContext *sws = NULL;
  SwrContext *swr = NULL;
  AVPacket *pkt = NULL;
  AVFrame *fr = NULL;
  AVDictionary *opts = NULL;
  int16_t *abuf = NULL;
  int abuf_cap = 0, vi = -1, ai = -1, rc, reading = 0;

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
  {                                                   /* list the audio tracks; honour a choice */
    int na = 0, cur = 0;
    pthread_mutex_lock(&p->mu);
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
      if (fmt->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
      if (na == p->want_audio) ai = (int)i;
      if ((int)i == ai) cur = na;
      if (na < 8) {
        AVDictionaryEntry *e = p_av_dict_get(fmt->streams[i]->metadata, "language", NULL, 0);
        snprintf(p->info.audio_lang[na], sizeof(p->info.audio_lang[na]), "%s", e && e->value ? e->value : "");
      }
      na++;
    }
    if (p->want_audio >= 0 && p->want_audio < na) cur = p->want_audio;
    p->info.naudio = na;
    p->info.audio_cur = cur;
    pthread_mutex_unlock(&p->mu);
  }
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
    p->info.fps = vsmooth_plausible(st->avg_frame_rate.num, st->avg_frame_rate.den);   /* 0: measured later */
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
  p->fmt = fmt;
  p->vi = vi;
  p->ai = ai;
  if (pthread_create(&p->reader, NULL, reader_main, p)) { set_error(p, "could not start reader thread", 0); goto done; }
  reading = 1;
  atomic_store(&p->state, PLAYER_BUFFERING);

  while (!atomic_load(&p->abort)) {
    int end_rc = 0;
    rc = queue_pop(p, pkt, &end_rc);
    if (rc < 0) break;
    if (rc > 0) {                                     /* played everything received */
      rc = end_rc;
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
        if (p->ngaps < VSMOOTH_GAPS && !isnan(pts) && !isnan(p->last_raw_vpts)) p->gaps[p->ngaps++] = pts - p->last_raw_vpts;
        p->last_raw_vpts = pts;
        if (wait_space(p, 1, 0)) break;
        int ox, oy, ow, oh;
        AVRational sar = fr->sample_aspect_ratio.num ? fr->sample_aspect_ratio : st->sample_aspect_ratio;
        track_bars(p, fr);
        if (atomic_load(p->zoom)) zoom_crop(p, fr, sar);
        fit_rect(fr, sar, atomic_load(p->margin), &ox, &oy, &ow, &oh);
        /* Fast path for streams pre-scaled on the server (Dispatcharr output
         * profile): same size in and out, so swscale takes its unscaled
         * YUV->RGB converter and there are no borders to clear. */
        int native = fr->width == PLAYER_W && fr->height == PLAYER_H && ow == PLAYER_W && oh == PLAYER_H;
        sws = p_sws_getCachedContext(sws, fr->width, fr->height, (enum AVPixelFormat)fr->format,
                                     ow, oh, AV_PIX_FMT_BGR0, native ? SWS_POINT : SWS_BILINEAR, NULL, NULL, NULL);
        if (sws) {
          pthread_mutex_lock(&p->mu);
          vslot *slot = &p->vq[(p->vhead + p->vcount) % VQ];
          p->info.native = native;
          /* a quick re-tune can open before the stream info is known */
          if (!p->info.width) { p->info.width = fr->width; p->info.height = fr->height; }
          if (p->info.fps <= 0) {                     /* declared late, or never: measure it */
            double f = vsmooth_plausible(st->avg_frame_rate.num, st->avg_frame_rate.den);
            if (f <= 0 && p->ngaps >= VSMOOTH_GAPS) f = vsmooth_rate(p->gaps, p->ngaps);
            if (f > 0) p->info.fps = f;
          }
          double fps = p->info.fps;
          pthread_mutex_unlock(&p->mu);
          if (ow != PLAYER_W || oh != PLAYER_H) memset(slot->px, 0, (size_t)PLAYER_W * PLAYER_H * 4);
          uint8_t *dst[4] = { (uint8_t *)(slot->px + (size_t)oy * PLAYER_W + ox), NULL, NULL, NULL };
          int dstride[4] = { PLAYER_W * 4, 0, 0, 0 };
          p_sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dst, dstride);
          pthread_mutex_lock(&p->mu);
          slot->pts = vsmooth_pts(&p->vsm, pts, fps);
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
  if (reading) {                                      /* stop the reader before its demuxer goes */
    atomic_store(&p->rstop, 1);
    pthread_mutex_lock(&p->qmu);
    pthread_cond_broadcast(&p->qcv);
    pthread_mutex_unlock(&p->qmu);
    pthread_join(p->reader, NULL);
  }
  queue_clear(p);
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

static void stream_close(stream *p);
static void stream_free(stream *p);

static stream *stream_create(atomic_int *zoom, atomic_int *margin)
{
  stream *p = calloc(1, sizeof(*p));
  if (!p) return NULL;
  pthread_mutex_init(&p->mu, NULL);
  pthread_cond_init(&p->cv, NULL);
  pthread_mutex_init(&p->qmu, NULL);
  pthread_cond_init(&p->qcv, NULL);
  p->aq = calloc((size_t)AQ_FRAMES * 2, sizeof(int16_t));
  p->shown = calloc((size_t)PLAYER_W * PLAYER_H, 4);
  for (int i = 0; i < VQ; i++) p->vq[i].px = calloc((size_t)PLAYER_W * PLAYER_H, 4);
  int ok = p->aq && p->shown;
  for (int i = 0; i < VQ; i++) ok = ok && p->vq[i].px;
  if (!ok) { stream_free(p); return NULL; }
  p->zoom = zoom;
  p->margin = margin;
  return p;
}

static void stream_free(stream *p)
{
  if (!p) return;
  stream_close(p);
  for (int i = 0; i < VQ; i++) free(p->vq[i].px);
  free(p->aq);
  free(p->shown);
  free(p->url);
  pthread_cond_destroy(&p->cv);
  pthread_mutex_destroy(&p->mu);
  pthread_cond_destroy(&p->qcv);
  pthread_mutex_destroy(&p->qmu);
  free(p);
}

static void reset_buffers(stream *p)
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
  memset(&p->stats, 0, sizeof(p->stats));
  aspect_track_reset(&p->bars);
  p->vframes = 0;
  vsmooth_reset(&p->vsm);
  p->ngaps = 0;
  p->last_raw_vpts = NAN;
  atomic_store(&p->rstop, 0);
  p->qend = p->qend_rc = 0;
  atomic_store(&p->rx_bytes, 0);
  atomic_store(&p->paused, 0);
  atomic_store(&p->shifted, 0);
  p->behind = 0;
}

static int stream_open(stream *p, const char *url, int want_audio)
{
  p->want_audio = want_audio;
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

static void stream_close(stream *p)
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


static void stream_get_stats(stream *p, player_stats *st)
{
  pthread_mutex_lock(&p->mu);
  *st = p->stats;
  pthread_mutex_unlock(&p->mu);
}

static int stream_attempt(stream *p, char *why, size_t n)
{
  pthread_mutex_lock(&p->mu);
  if (why && n) snprintf(why, n, "%s", p->attempt_err);
  pthread_mutex_unlock(&p->mu);
  return atomic_load(&p->attempt);
}

static void stream_get_info(stream *p, player_info *info)
{
  pthread_mutex_lock(&p->mu);
  *info = p->info;
  pthread_mutex_unlock(&p->mu);
}

static double pts_at(stream *p, uint64_t pos)
{
  /* latest anchor at or before pos */
  int lo = p->nanc > NANCHOR ? p->nanc - NANCHOR : 0;
  for (int i = p->nanc - 1; i >= lo; i--) {
    anchor *a = &p->anc[i % NANCHOR];
    if (a->pos <= pos) return a->pts + (double)(pos - a->pos) / PLAYER_RATE;
  }
  return NAN;
}

static void stream_audio(stream *p, int16_t *out, size_t frames, double frame_seconds)
{
  memset(out, 0, frames * 2 * sizeof(int16_t));
  int st = atomic_load(&p->state);
  if (st != PLAYER_BUFFERING && st != PLAYER_PLAYING) return;
  if (atomic_load(&p->paused)) return;               /* held: the clock stands still */

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

static int stream_video(stream *p, uint32_t *dst)
{
  pthread_mutex_lock(&p->mu);
  int st = atomic_load(&p->state);
  int advanced = 0;
  if (st == PLAYER_PLAYING && !atomic_load(&p->paused)) {
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
      if (advanced) p->stats.dropped++;
      if (p->clock_valid && !isnan(head->pts) && fabs(head->pts - p->clock) <= 1.0) {
        double sk = fabs(p->clock - head->pts);
        p->stats.skew_sum += sk;
        if (sk > p->stats.skew_max) p->stats.skew_max = sk;
      }
      p->stats.shown++;
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

static void stream_pause(stream *p, int on)
{
  if (on == atomic_load(&p->paused)) return;
  if (on) {
    if (atomic_load(&p->state) != PLAYER_PLAYING) return;
    atomic_store(&p->shifted, 1);
    p->pause_start = now_mono();
    p->rx_at_pause = atomic_load(&p->rx_bytes);
    atomic_store(&p->paused, 1);
    return;
  }
  double d = now_mono() - p->pause_start;
  p->behind += d;
  pthread_mutex_lock(&p->mu);
  p->play_start_mono += d;                           /* streams without audio keep their place too */
  pthread_mutex_unlock(&p->mu);
  atomic_store(&p->paused, 0);
}

static void stream_get_shift(stream *p, player_shift *s)
{
  memset(s, 0, sizeof(*s));
  s->room = -1;
  int64_t total = atomic_load(&shift_total);
  s->full = total >= SHIFT_MAX - SHIFT_SLACK;
  s->behind = p->behind;
  if (!atomic_load(&p->paused) || atomic_load(&p->state) != PLAYER_PLAYING) return;
  s->paused = 1;
  s->paused_for = now_mono() - p->pause_start;
  s->behind += s->paused_for;
  double rate = s->paused_for >= 3 ? (double)(atomic_load(&p->rx_bytes) - p->rx_at_pause) / s->paused_for : 0;
  if (rate > 0) s->room = (double)(SHIFT_MAX - SHIFT_SLACK - total) / rate;
  if (s->room < 0 && rate > 0) s->room = 0;
}

/* ---------- player: the current stream, old ones closed in the background ---------- */

/* Closing a stream (joining its thread, freeing the demuxer and FFmpeg's
 * decoder threads) took up to ~100 ms on the Pi. A channel change used to do
 * that inside retro_run; now the old stream is told to stop and a detached
 * reaper thread does the rest. player_destroy waits for the reapers, because
 * the frontend may unload the core right after. */
struct player {
  stream *cur;
  atomic_int zoom;
  atomic_int margin;
  pthread_mutex_t rmu;
  pthread_cond_t rcv;
  int reaping;                  /* streams still being closed */
};

typedef struct { player *pl; stream *s; } reap_job;

static void *reap_main(void *arg)
{
  reap_job *j = arg;
  stream_free(j->s);
  pthread_mutex_lock(&j->pl->rmu);
  j->pl->reaping--;
  pthread_cond_broadcast(&j->pl->rcv);
  pthread_mutex_unlock(&j->pl->rmu);
  free(j);
  return NULL;
}

static void retire(player *pl, stream *s)
{
  if (!s) return;
  atomic_store(&s->abort, 1);                       /* stop network and decoding now */
  pthread_mutex_lock(&s->mu);
  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&s->mu);
  reap_job *j = malloc(sizeof(*j));
  pthread_t t;
  pthread_attr_t a;
  int ok = 0;
  if (j && !pthread_attr_init(&a)) {
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    *j = (reap_job){ pl, s };
    pthread_mutex_lock(&pl->rmu);
    pl->reaping++;
    pthread_mutex_unlock(&pl->rmu);
    ok = !pthread_create(&t, &a, reap_main, j);
    pthread_attr_destroy(&a);
    if (!ok) {
      pthread_mutex_lock(&pl->rmu);
      pl->reaping--;
      pthread_mutex_unlock(&pl->rmu);
    }
  }
  if (!ok) { free(j); stream_free(s); }             /* fall back to closing here */
}

player *player_create(void)
{
  player *pl = calloc(1, sizeof(*pl));
  if (!pl) return NULL;
  pthread_mutex_init(&pl->rmu, NULL);
  pthread_cond_init(&pl->rcv, NULL);
  return pl;
}

void player_destroy(player *pl)
{
  if (!pl) return;
  stream_free(pl->cur);
  pl->cur = NULL;
  pthread_mutex_lock(&pl->rmu);
  while (pl->reaping > 0) pthread_cond_wait(&pl->rcv, &pl->rmu);
  pthread_mutex_unlock(&pl->rmu);
  pthread_cond_destroy(&pl->rcv);
  pthread_mutex_destroy(&pl->rmu);
  free(pl);
}

int player_open_track(player *pl, const char *url, int audio_track)
{
  retire(pl, pl->cur);
  pl->cur = stream_create(&pl->zoom, &pl->margin);
  if (!pl->cur) return -1;
  return stream_open(pl->cur, url, audio_track);
}

int player_open(player *pl, const char *url) { return player_open_track(pl, url, -1); }

int player_reconnect(player *pl)
{
  stream *old = pl->cur;
  if (!old || !old->url) return -1;
  stream *s = stream_create(&pl->zoom, &pl->margin);
  if (!s) return -1;
  pthread_mutex_lock(&old->mu);
  int had = old->have_shown;
  if (had) memcpy(s->shown, old->shown, (size_t)PLAYER_W * PLAYER_H * 4);
  int track = old->info.naudio > 1 ? old->info.audio_cur : old->want_audio;
  pthread_mutex_unlock(&old->mu);
  char *url = strdup(old->url);
  retire(pl, old);
  pl->cur = s;
  if (!url) return -1;
  int rc = stream_open(s, url, track);
  free(url);
  pthread_mutex_lock(&s->mu);
  s->have_shown = had;                               /* the last picture stays up meanwhile */
  pthread_mutex_unlock(&s->mu);
  return rc;
}

void player_close(player *pl)
{
  retire(pl, pl->cur);
  pl->cur = NULL;
}

player_state player_get_state(player *pl) { return pl->cur ? (player_state)atomic_load(&pl->cur->state) : PLAYER_IDLE; }
const char *player_error(player *pl) { return pl->cur ? pl->cur->err : ""; }
double player_tuning_seconds(player *pl) { return pl->cur ? now_mono() - pl->cur->open_time : 0.0; }
void player_set_zoom(player *pl, int zoom) { atomic_store(&pl->zoom, zoom ? 1 : 0); }
void player_set_margin(player *pl, int percent) { atomic_store(&pl->margin, percent < 0 ? 0 : percent > 20 ? 20 : percent); }

void player_get_stats(player *pl, player_stats *st)
{
  if (pl->cur) stream_get_stats(pl->cur, st);
  else memset(st, 0, sizeof(*st));
}

int player_attempt(player *pl, char *why, size_t n)
{
  if (pl->cur) return stream_attempt(pl->cur, why, n);
  if (why && n) why[0] = '\0';
  return 0;
}

void player_get_info(player *pl, player_info *info)
{
  if (pl->cur) stream_get_info(pl->cur, info);
  else memset(info, 0, sizeof(*info));
}

void player_audio(player *pl, int16_t *out, size_t frames, double frame_seconds)
{
  if (pl->cur) stream_audio(pl->cur, out, frames, frame_seconds);
  else memset(out, 0, frames * 2 * sizeof(int16_t));
}

int player_video(player *pl, uint32_t *dst) { return pl->cur ? stream_video(pl->cur, dst) : 0; }

void player_pause(player *pl, int on) { if (pl->cur) stream_pause(pl->cur, on ? 1 : 0); }

void player_get_shift(player *pl, player_shift *s)
{
  if (pl->cur) stream_get_shift(pl->cur, s);
  else { memset(s, 0, sizeof(*s)); s->room = -1; }
}
