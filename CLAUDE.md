# CLAUDE.md — RetroTV libretro core

Guidance for Claude Code (and humans) working in this repo.

## What this is

RetroTV is a **libretro core that plays live IPTV channels from an M3U playlist
like a 90s cable box**: channel up and down, a channel banner, a channel list,
static while tuning, and "NO SIGNAL" on a dead channel. The target is
**ReplayOS on a Raspberry Pi 5**, where Alpha Player can't play stream URLs.
The source of the streams is Dispatcharr.

Status: **prototype**. The core runs under our own headless test frontend
(`tests/harness.c`) on the real Pi. It is **not installed into ReplayOS yet**
(see "Deploying into ReplayOS" below).

## Verified facts about the target (checked on the real Pi, Oct 2026)

- Raspberry Pi 5, **ReplayOS v2.0.60**, Debian 13 (trixie) userland, **glibc 2.41**, aarch64.
- No compiler on the Pi, but the FFmpeg 7.1 shared libraries are present:
  `libavformat.so.61` (61.7.103), `libavcodec.so.61`, `libavutil.so.59`,
  `libswscale.so.8`, `libswresample.so.5`. Their input protocols include
  http, https, hls, udp, tcp, rtmp and srt.
- ReplayOS cores live in `/opt/replay/cores/*.so`. The system → core mapping is
  in `/opt/replay/cores/cores.cfg` (`[alpha_player] low/mid/hi = "..."`).
- The ReplayOS frontend is `/opt/replay/replay` (`replay.service`). Its REST
  API is on port 55356, with the token in `/media/sd/config/replay.cfg`
  (`replay_http_token`). Never hard-code or commit tokens.
- Alpha Player rejects URL entries in `.m3u` files, and lists of local files work.
  Channel flipping via named pipes (FIFOs) works in Alpha Player, but seeking
  (L/R) shows "Not supported" on a pipe.
- Dispatcharr streams are MPEG-TS over HTTP. A cold channel can take ~3–6 s
  to deliver its first byte.

## Build & test

```bash
make deps        # FFmpeg n7.1 headers -> deps/ffmpeg (headers only, git-ignored)
make test        # host unit tests (m3u parser, overlay) with ASan/UBSan
make check       # host compile of all sources with -Werror
make             # cross-build build/retrotv_libretro.so + build/harness for aarch64 (zig)
make pi-test     # run on the Pi via SSH, screenshots -> build/pi-shots/
```

- Cross-compiler: `pip install ziglang==0.13.0`. The Makefile uses
  `python3 -m ziglang cc -target aarch64-linux-gnu.2.36`.
- `make pi-test` needs SSH key access to `root@<pi>` (`PI=` overrides) and a
  private playlist at `tests/local/channels.m3u` (git-ignored). Include one dead
  URL so the NO SIGNAL path gets tested.
- **Always look at the screenshots** in `build/pi-shots/` after a pi-test; the
  log alone doesn't prove anything is on screen.

## Architecture

| File | Role |
|---|---|
| `src/retrotv.c` | libretro entry points, input, UI state (banner, list, static, no-signal) |
| `src/player.c` | one stream: FFmpeg demux/decode thread → video ring (6 frames) + audio ring (3 s); audio is the master clock |
| `src/ffapi.c/.h` | FFmpeg functions resolved with `dlopen` by soname. To use a new FFmpeg function, add it to `RETROTV_FF_FUNCS` and call it as `p_<name>` |
| `src/m3u.c/.h` | IPTV M3U parser (`#EXTINF` tvg-* attributes, `x-tvg-url`, relative paths) |
| `src/osd.c/.h` | software overlay on the XRGB8888 framebuffer, 8x8 public-domain font |
| `tests/harness.c` | headless libretro frontend: real-time pacing, scripted button presses, PNG screenshots |

Output is fixed at **640x480 XRGB8888, 4:3, 60 fps, 48 kHz S16 stereo** (CRT
friendly). Video is scaled to fit with letter- or pillar-boxing. When the
timestamps jump (channel splices or ad breaks), playback re-syncs instead of
freezing.

## Rules

- **Don't modify ReplayOS** (`/opt/replay`, `cores.cfg`, `replay.cfg`) unless
  the user asks for that specific change, and back up anything you touch first.
  Test with the harness from `/tmp` instead.
- FFmpeg ABI is pinned to 7.1 (sonames .61/.59/.8/.5). If ReplayOS changes
  FFmpeg major versions, update `FFMPEG_TAG` and the sonames in `ffapi.c` together.
- `retro_run` must never block. Network and decoding happen only on the player thread.
- Never commit playlists, stream URLs, tokens or screenshots of private channels.
- Keep `make test` and `make check` passing (CI runs both and the cross-build).

## Deploying into ReplayOS (not done yet — needs the user's go-ahead)

ReplayOS has a fixed list of systems, so the only way in today is to take over
an existing system's core, most likely `alpha_player`:

1. Back up `/opt/replay/cores/cores.cfg`.
2. Copy `retrotv_libretro.so` into `/opt/replay/cores/`.
3. Point `[alpha_player]` at it.

The catches: this replaces Alpha Player for local video, ReplayOS updates
probably overwrite both files, and ReplayOS may check file extensions itself.
Investigate before doing it, and prefer asking the ReplayOS author for an
official hook.

## Roadmap and design

- `docs/ROADMAP.md` — milestones and QoL ideas.
- `docs/DESIGN.md` — the retro cable TV look: guide grid, now-playing banner,
  options. Mockups are linked there.
