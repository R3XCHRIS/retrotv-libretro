# CLAUDE.md — RetroTV libretro core

Guidance for Claude Code (and humans) working in this repo.

## What this is

RetroTV is a **libretro core that plays live IPTV channels from an M3U playlist
like a 90s cable box**: channel up and down, a channel banner, a channel list,
static while tuning, and "NO SIGNAL" on a dead channel. The target is
**ReplayOS on a Raspberry Pi 5**, where Alpha Player can't play stream URLs.
The source of the streams is Dispatcharr.

Status: **prototype**. It runs under our headless test frontend
(`tests/harness.c`) and, since Oct 2026, **inside ReplayOS on the user's Pi via
the `avtest` slot** (Extra → audio_video_test; see "Deploying into ReplayOS" below).

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
- Dispatcharr streams are MPEG-TS over HTTP (H.264 + AAC/AC-3; some PAL 50 fps).
  A cold channel can take ~3–10 s to deliver its first byte, and occasionally
  fails one attempt with "Invalid data found when processing input". The core retries.
- Measured on the Pi with the harness: tune-to-picture 4–7 s cold and ~0.1 s warm,
  `retro_run` 1–3 ms normally, at most 7 threads (no leak across channel changes).

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

## Dispatcharr setup (stream source)

Keep the core source-agnostic: it reads plain M3U + XMLTV. Dispatcharr-specific
features are configured in Dispatcharr and selected through the playlist URL:

- **Channel Profile** = the subset of channels RetroTV shows (`/output/m3u/<profile>`;
  the EPG link `/output/epg/<profile>` arrives as `x-tvg-url` in the M3U header).
- **Output Profile** = an ffmpeg pre-transcode (`pipe:0` → MPEG-TS `pipe:1`) that crops
  and downscales to 640x480 on the server. It's selected with `?output_profile=<id>` on the
  M3U link (exists in v0.32.0; `core.models.OutputProfile`). **Not used by default:** the
  user's Dispatcharr already transcodes every stream with a VAAPI stream profile, so this
  would be a second transcode (slower tunes). The deployed playlist is the direct link.
  A wrong or inactive id silently falls back to passthrough: verify with ffprobe.
- Use the Dispatcharr REST API only for things the M3U can't do (e.g. resetting a stuck
  channel), and only behind a token kept out of the repo.

## Architecture

| File | Role |
|---|---|
| `src/retrotv.c` | libretro entry points, input, UI state and screens (banner, list, X menu, setup, welcome), sleep timer, guide fetch thread, saved state |
| `src/player.c` | a stream per tune: a reader thread (FFmpeg demux → packet queue) and a decode thread → video ring (6 frames) + audio ring (3 s), audio is the master clock; old streams closed by a reaper thread; zoom crop, overscan margin, audio track choice. Pause (timeshift): the consumer stops while packets keep queuing in RAM, one 1 GiB budget for all streams (`SHIFT_MAX`, ~13 min at 10 Mbit/s); full → plays on by itself |
| `src/ffapi.c/.h` | FFmpeg functions resolved with `dlopen` by soname. To use a new FFmpeg function, add it to `RETROTV_FF_FUNCS` and call it as `p_<name>` |
| `src/m3u.c/.h` | IPTV M3U parser (`#EXTINF` tvg-* attributes, `x-tvg-url`, relative paths) |
| `src/plconf.c` | playlist.txt: the playlist line and `epg=` |
| `src/remote.c/.h` | remote control: tiny HTTP server thread on port 7474 (`/tune?ch=`, `/up`, `/down`, `/last`, `/status`, `/channels` with logos and XMLTV `<icon>` posters, `/settings`, `/set?sleep=&audio=&leveller=&picture=&output=`); requests are queued and applied in `retro_run`, settings through the X menu's own setters (a snapshot is published for the server thread under `remote_mu`). `RETROTV_REMOTE_PORT` overrides the port (tests beside an installed RetroTV) |
| `src/webui.html` → `src/webui.c` | the phone remote page served at `/`; edit the HTML, then `make src/webui.c` (`make check` fails if they differ). Self-contained: no external fonts or scripts |
| `src/epg.c` | XMLTV parser (time window, entities, display names), `epg_lookup`, clock and title helpers |
| `src/gunzip.c` | `.xml.gz` guides via the system zlib (`dlopen`) |
| `src/guide.c` | TV guide screen |
| `src/options.c` | core options table (v2 with v0 fallback) → `rtv_options` |
| `src/osd.c/.h`, `src/theme.c` | software overlay on XRGB8888 (8x8 public-domain font; `vshift` for 240p), tuning static and the paused-VHS effect (`osd_vhs_pause`, applied in place to the held frame every frame), colour themes |
| `src/vsmooth.c` | video timestamp smoothing (jittery sources broke the 2-3 cadence of 24 fps on 60 Hz) and frame-rate checks: a declared rate outside 5-120 fps (e.g. a 90 kHz "tbr") is ignored and the rate is measured from frame gaps |
| `src/aspect.c`, `src/loud.c`, `src/lastch.c`, `src/onboard.c`, `src/testcard.c` | bar detection and zoom crop, volume leveller (AGC), last channel and per-channel zoom, setup-screen hints, test card |
| `tests/harness.c` | headless libretro frontend: real-time pacing, scripted buttons, PNG screenshots, `HARNESS_OPTS`, save dir |
| `tests/demo/` | demo line-up for screenshots: made-up channels, generated video and guide |

Every pure module has host tests in `tests/test_*.c` (`make test`).

Output is **640x480 XRGB8888, 4:3** (or 640x240 with the 240p option), 60 Hz or
50 Hz for 25/50 fps channels, **48 kHz S16 stereo** (CRT
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

## Deploying into ReplayOS (avtest slot — installed on the user's Pi)

ReplayOS has a hardcoded system list and `_extra` only accepts `.lr`/`.sh`, so
RetroTV borrows the **`[avtest]`** slot (Extra → audio_video_test). Verified on
ReplayOS 2.0.60 (Oct 2026), with sound.

- **Don't use `[pibench]`: ReplayOS mutes that slot.** The core's audio is accepted
  (`av_enable=3`, every frame taken) but nothing reaches the speaker; the same core
  in `[avtest]` has sound. Older installs used pibench; the installer gives it back.
- No new Extras entry is possible: `pibench.lr`/`audio_video_test.lr` are hardcoded,
  and `/api/v1/load_game` rejects those special systems ("Invalid system").
- ReplayOS keeps `replay.cfg` in memory and writes it on shutdown: never edit it while
  `replay.service` runs (the edit gets overwritten). Change settings from its menu.
  If storage silently falls back to `sd`, the systems list empties; reset it in the menu.

- `replayos/retrotv_install.sh` (in `<storage>/roms/_extra/`, runs as root from
  the Extra menu or SSH): copies `<storage>/retrotv/retrotv_libretro.so` to
  `/opt/replay/cores/`, rewrites only `[avtest]` low/mid/hi (and gives `[pibench]`
  back if an older install took it), backs up
  `cores.cfg` to `<storage>/retrotv/backup/` and `cores.cfg.pre-retrotv`,
  restarts `replay.service`. Re-run after every ReplayOS update (A/B system
  slot replaces `/opt/replay`).
- `replayos/retrotv_uninstall.sh` restores `[avtest]` (and `[pibench]`) and removes the core.
- Storage backups use plain `cp`: the NFS share squashes root, so `cp -p` fails.
- ReplayOS launches the core with `<storage>/roms/_extra/audio_video_test.lr`. The core then finds its playlist in this order: launched
  `.m3u`, first line of the launched file, `/media/{nfs,usb,nvme,sd}/retrotv/playlist.txt`
  (first line: URL or path), then `.../retrotv/channels.m3u`. No playlist → help screen.
- State: the last channel is saved in the frontend's save directory, which ReplayOS sets to
  `<storage>/saves/<system>/` (now `saves/avtest/retrotv-last-<playlist hash>.txt`). Moving
  slots means one fresh start. The EPG comes from the playlist's `x-tvg-url`, or from an
  `epg=<URL or path>` line in playlist.txt (overrides it; `.xml.gz` unpacked with the system
  zlib via dlopen). Matching: tvg-id, then display name, then number (own guide only).
  Tunarr works too: use `/api/channels.m3u` (HLS streams, guide at `/api/xmltv.xml`), not
  `/stream/channels/N.m3u8` (that's one channel's video).
- Core options: ReplayOS accepts core options v2 and lists them in its in-game menu as
  **System Settings** (next to System Input, System Media, Replay Options), with a save
  entry; changes reach the core live (GET_VARIABLE_UPDATE).
- Logs: `/tmp/retrotv.log` on the Pi. API: `http://<pi>:55356/api/v1/get_status`
  and `/api/v1/set_cmd?cmd=screenshot` (header `X-RePlay-Token`) → captures in
  `<storage>/captures/<system>/` (e.g. `/media/nfs/captures/avtest/`). `get_info` reports the display refresh (50/60 Hz).
- Users install with the one-liner (`install.sh` at the repo root, `curl … | bash` on the Pi as root):
  it downloads the latest GitHub Release, checks `SHA256SUMS`, copies the core and the
  Extras scripts (install, uninstall, update) and runs `retrotv_install.sh`. Releases are
  made by `.github/workflows/release.yml` when a `v*` tag is pushed; bump `RETROTV_VERSION`.
  Test the installer against a local copy of the release with `RETROTV_BASE=file:///dir`.
- Screenshots for the docs: `PI=root@<pi> scripts/demo-shots.sh` (demo line-up, never real channels).
- Autostart (verified 2.0.60): ReplayOS reads a file in `<storage>/roms/_autostart/` at start
  (we used the name `audio_video_test.lr`). Its content is a path appended to the storage root
  with no slash, so it starts with `/`: `/roms/avtest/retrotv.lr`. The core comes from the
  parent folder's name → `cores.cfg` section, which is why `_extra/...` fails ("Failed to
  identify the core") and an empty `roms/avtest/retrotv.lr` works. Errors: `/var/log/replay.log`.
- Zaparoo Core (user-installed in `/media/sd/zaparoo/`, API on 7497): its `**launch:` can't
  start Extras; use autostart. Its virtual gamepad is player 2 (RetroTV reads players 1-4),
  and menus need ReplayOS's "all players control the UI". Tags tune with
  `**http.get:http://127.0.0.1:7474/tune?ch=<n>` (RetroTV's remote control option).
- **Two screens** (verified 2026-10-17 with two CRTs on the Pi's HDMI ports): ReplayOS "Multi screen"
  (`video_monitor_multi_mode`, or per system/game `replay_video_monitor_multi_mode`) = Dual
  horizontal (2) splits one core picture: left half → screen 1, right half → screen 2. Rules learned
  the hard way: both screens connected at boot; **never change geometry or refresh while running**
  (SET_SYSTEM_AV_INFO → page flips fail, main CRT flashes until reboot); Dual smart applies only to
  dual-screen arcade games; per-game Dual horizontal + `_autostart` = ReplayOS segfault at start-up.
  Dual modes ReplayOS picks: 1280x480 → `1920x480i` (39.147 MHz, 2496x523) on both outputs;
  1280x240 → `1920x240p` (39.04 MHz, 2494x261). After any mode change the two outputs start with a
  random phase; out of step, ReplayOS runs at exactly half rate (retro_run every 33 ms). A
  re-sent identical mode is ignored, a 1-frame bounce doesn't fully restart the outputs, and a
  1 s bounce is only a coin toss, so the size is fixed per session and RetroTV just logs it.
  A loose RGB-Pi v2 HDMI-to-SCART adapter caused most of the "lost sync" flicker blamed on modes.
  RetroTV's `retrotv_dual` option (read at load) gives a fixed 1280x480 (or 1280x240 with Output 240p) at 60 Hz: the globals are the
  focused screen, `other` the second; `exchange()` swaps them (see the comment in `src/retrotv.c`).
- **Virtual Boy slot** (`--slot vb|both`, verified 2026-10-17): `[nintendo_vb]` → RetroTV plus an empty
  `roms/nintendo_vb/RetroTV.vb`; `GET /api/v1/load_game?system=nintendo_vb&game_file=RetroTV.vb`
  (token header) starts it from the menu: no autostart, no restart. Settings and saves are per
  system (`config/settings/system/<crt|lcd>/nintendo_vb.cfg`, `saves/nintendo_vb/`).
  `replayos/retrotv_launch.sh` (installed to **/media/sd/retrotv/**) starts and tunes it.
  Lessons: Zaparoo's launch on ReplayOS first "stops the active launcher" = **restarts
  replay.service**; a script running from `/media/nfs` during that restart keeps the share busy
  and ReplayOS falls back to `system_storage = "sd"` (systems list empties). So the launcher
  lives on the SD card and channel cards use `**execute` (allow_execute), not launches.
  `audio_system_volume = "0"` in replay.cfg means volume **Off** (scale 10…0).
- **ReplayOS 2.1.0 external systems** (the author's user-cores hook; docs received 2026-10-29,
  not yet tested on the Pi, which runs 2.0.60): a `retrotv_libretro.cfg` next to the core in
  `/opt/replay/cores/` (four quoted fields: `id = "retrotv"`, `name`, `extensions = "m3u|m3u8|txt"`,
  `devices = "pi5"`; unknown or repeated fields are rejected; regular files, no symlinks) makes
  RetroTV its own system, read only at frontend start-up (restart RePlay). Its "games" are the files in
  `roms/retrotv/`: RetroTV already takes a launched `.m3u`, a `.txt` like playlist.txt, or a
  comment-only `.txt` (→ `retrotv/playlist.txt`). Saves/settings use the id (`saves/retrotv/`,
  `config/settings/system/<crt|lcd>/retrotv.cfg`). OTA updates keep unowned files in the cores
  folder. An external system can't pair with a library `cores.cfg` uses, so the installer gives
  the borrowed slots back. `retrotv_install.sh --slot system` (default on 2.1.0+, from
  `dpkg-query replay-frontend`) does all this, seeds settings/saves from nintendo_vb/avtest and
  restarts via `systemd-run` (nothing left running on NFS). Launcher: `load_game?system=retrotv&
  game_file=RetroTV.txt`. `tests/test_install.sh` tests the scripts in a fake root (`unshare -r`).

## Roadmap and design

- `docs/ROADMAP.md` — milestones and QoL ideas.
- `docs/DESIGN.md` — the retro cable TV look: guide grid, now-playing banner,
  options. Mockups are linked there.
