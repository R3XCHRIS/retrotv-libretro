/* player.h - one live stream: FFmpeg demux/decode on a background thread,
 * pulled by the libretro core once per frame. Audio is the master clock. */
#ifndef RETROTV_PLAYER_H
#define RETROTV_PLAYER_H

#include <stddef.h>
#include <stdint.h>

#define PLAYER_W 640
#define PLAYER_H 480
#ifndef PLAYER_RATE
#define PLAYER_RATE 48000          /* output audio: S16 stereo */
#endif

typedef enum {
  PLAYER_IDLE = 0,
  PLAYER_CONNECTING,   /* opening the URL / probing streams */
  PLAYER_BUFFERING,    /* decoding, waiting for enough audio+video */
  PLAYER_PLAYING,
  PLAYER_ERROR,
} player_state;

typedef struct {
  char vcodec[32], acodec[32];
  int width, height;
  double fps;
  int native;          /* frames arrive at PLAYER_W x PLAYER_H square-pixel: no resize */
  int bars_top, bars_left;  /* black bars detected in the stream (pixels) */
  int naudio;               /* audio tracks in the stream */
  int audio_cur;            /* which one plays (0-based among the audio tracks) */
  char audio_lang[8][8];    /* their language tags ("eng", "" if none) */
} player_info;

typedef struct {
  long shown, dropped;      /* video frames that came due / of those, skipped for a newer one */
  double skew_sum, skew_max; /* |audio clock - pts| over the frames that came due, seconds */
} player_stats;

typedef struct player player;

player *player_create(void);
void player_destroy(player *p);

/* Start tuning a URL (returns immediately; work happens on the thread). */
int player_open(player *p, const char *url);
/* Same, preferring the audio_track-th audio track (0-based; -1 = the best one). */
int player_open_track(player *p, const char *url, int audio_track);
/* Open the same URL again (same audio track) after a live stream ended or
 * dropped, showing the last picture until the new one plays. -1 if there's
 * nothing to reopen. */
int player_reconnect(player *p);
/* Stop and join the thread. Safe to call when idle. */
void player_close(player *p);

player_state player_get_state(player *p);
const char *player_error(player *p);          /* valid while state == PLAYER_ERROR */
void player_get_info(player *p, player_info *info);
/* ZOOM (1) crops the picture inside any black bars to fill 4:3; FIT (0) shows
 * the whole frame. Takes effect on the next decoded frame; kept across tunes. */
void player_set_zoom(player *p, int zoom);
/* Shrink the video into a margin of percent (0..20) on every side, for
 * screens that cut off the edges. Takes effect on the next frame. */
void player_set_margin(player *p, int percent);

/* Playback stats since the last player_open (for the log and measurements). */
void player_get_stats(player *p, player_stats *st);
double player_tuning_seconds(player *p);      /* time since player_open */
/* Current connection attempt (1-based); why = reason the previous one failed. */
int player_attempt(player *p, char *why, size_t n);

/* Fill exactly `frames` stereo frames into out (silence when nothing to play).
 * Advances the playback clock. Call once per retro_run before player_video. */
void player_audio(player *p, int16_t *out, size_t frames, double frame_seconds);
/* Copy the frame due at the current clock into dst (PLAYER_W x PLAYER_H XRGB8888).
 * Returns 1 if dst holds a picture, 0 if there is nothing yet. */
int player_video(player *p, uint32_t *dst);

/* Pause (timeshift) a playing stream: the picture and sound hold while the
 * stream keeps arriving into memory; play carries on from the same moment,
 * now behind live. A new tune (player_open) goes back to live. */
typedef struct {
  int paused;
  double paused_for;   /* seconds, this pause */
  double behind;       /* seconds behind live, all pauses since the tune */
  double room;         /* seconds more it can stay paused at this stream's rate (-1 = not known yet) */
  int full;            /* the timeshift memory (shared by every stream) is full: resume */
} player_shift;
void player_pause(player *p, int on);
void player_get_shift(player *p, player_shift *s);

#endif
