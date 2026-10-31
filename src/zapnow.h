/* zapnow.h - tell Zaparoo Core (on the same device) what RetroTV is showing,
 * so its app has something under "now playing". Zaparoo doesn't know RetroTV's
 * core, so RetroTV reports the channel itself, as a Video item, through
 * Zaparoo's JSON-RPC API (POST /api; requests from this device need no pairing).
 * A background thread sends the newest text; retro_run never waits on it. */
#ifndef RETROTV_ZAPNOW_H
#define RETROTV_ZAPNOW_H

#include <stddef.h>

#define ZAPNOW_PORT 7497

/* The JSON-RPC body for media.active.update: system Video, mediaName name,
 * mediaPath "retrotv://<path>". Returns the length, or -1 if out doesn't fit. */
int zapnow_body(char *out, size_t n, const char *name, const char *path);

/* Report name (e.g. "RetroTV: 36 Star Trek"), with a stable path (the channel
 * number). Only the newest report is kept; an unchanged one isn't sent again.
 * Starts the sender on first use. port 0 = ZAPNOW_PORT. */
void zapnow_report(const char *name, const char *path, int port);

/* The outcome of the last send that differs from the one before ("" when
 * nothing new): for the log, from the main thread. */
void zapnow_take_note(char *out, size_t n);

/* Stop the sender (joins it). Safe to call when it never started. */
void zapnow_stop(void);

#endif
