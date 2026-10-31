# CLAUDE.md — RetroTV libretro core

Guidance for Claude Code (and humans) working in this repo.

## What this is

RetroTV is a **libretro core that plays live IPTV channels from an M3U playlist
like a 90s cable box**: channel up and down, a channel banner, a channel list,
static while tuning, and "NO SIGNAL" on a dead channel. The target is
**ReplayOS on a Raspberry Pi 5**, where Alpha Player can't play stream URLs.
The source of the streams is Dispatcharr.

Status: **released** (GitHub Releases, one-line installer). Since v0.3.0 it installs as its
own ReplayOS system (ReplayOS 2.1.0 external systems) and runs on the user's Pi; the
headless test frontend (`tests/harness.c`) runs it anywhere (see "Deploying into ReplayOS").

## Verified facts about the target (checked on the real Pi, Oct 2026)

- Raspberry Pi 5, **ReplayOS 2.1.0** (from 2.0.60 on 2026-10-31; version: `dpkg-query -W replay-frontend`),
  Debian 13 (trixie) userland, **glibc 2.41**, aarch64. RetroTV needs 2.1.0 or newer.
- No compiler on the Pi, but the FFmpeg 7.1 shared libraries are present:
  `libavformat.so.61` (61.7.103), `libavcodec.so.61`, `libavutil.so.59`,
  `libswscale.so.8`, `libswresample.so.5`. Their input protocols include
  http, https, hls, udp, tcp, rtmp and srt.
- ReplayOS cores live in `/opt/replay/cores/*.so`. Built-in systems map to cores in
  `/opt/replay/cores/cores.cfg`; external systems (RetroTV) register with a `<name>_libretro.cfg`
  next to their core instead (see below).
- The ReplayOS frontend is `/opt/replay/replay` (`replay.service`). Its REST
  API is on port 55356, with the token in `/media/sd/config/replay.cfg`
  (`replay_http_token`). Never hard-code or commit tokens.
- Alpha Player rejects URL entries in `.m3u` files, and lists of local files work.
  Channel flipping via named pipes (FIFOs) works in Alpha Player, but seeking
  (L/R) shows "Not supported" on a pipe.
- Dispatcharr streams are MPEG-TS over HTTP (H.264 + AAC/AC-3; some PAL 50 fps).
  A cold channel can take ~3–10 s to deliver its first byte, and occasionally
  fails one attempt with "Invalid data found when processing input". The core retries.
- Live streams can "end": a provider closes the session (Pluto via a public list with one shared
  deviceId serves an empty playlist with `#EXT-X-ENDLIST` after a while) or a server restarts.
  RetroTV reconnects a stream that was playing (`player_reconnect` keeps the last picture up);
  three drops within 10 s of play each → NO SIGNAL.
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
| `src/retrotv.c` | libretro entry points, input, UI state and screens (banner, list, X menu, setup, welcome), two screens, sleep timer, guide fetch thread, update thread, saved state |
| `src/player.c` | a stream per tune: a reader thread (FFmpeg demux → packet queue) and a decode thread → video ring (6 frames) + audio ring (3 s), audio is the master clock; old streams closed by a reaper thread; zoom crop, overscan margin, audio track choice. Pause (timeshift): the consumer stops while packets keep queuing in RAM, one 1 GiB budget for all streams (`SHIFT_MAX`, ~13 min at 10 Mbit/s); full → plays on by itself |
| `src/ffapi.c/.h` | FFmpeg functions resolved with `dlopen` by soname. To use a new FFmpeg function, add it to `RETROTV_FF_FUNCS` and call it as `p_<name>` |
| `src/m3u.c/.h` | IPTV M3U parser (`#EXTINF` tvg-* attributes, `x-tvg-url`, relative paths) |
| `src/plconf.c` | playlist.txt: the playlist line and `epg=` |
| `src/remote.c/.h` | remote control: tiny HTTP server thread on port 7474 (`/tune?ch=`, `/up`, `/down`, `/last`, `/pause` `/play` `/live`, `/focus`, `/swap`, `/update`, `/status`, `/channels` with logos and XMLTV `<icon>` posters, `/settings`, `/set?sleep=&audio=&leveller=&picture=&output=&screen2=`, `screen=` for two screens); requests are queued and applied in `retro_run`, settings through the X menu's own setters (a snapshot is published for the server thread under `remote_mu`). `RETROTV_REMOTE_PORT` overrides the port (tests beside an installed RetroTV) |
| `src/update.c/.h` | RetroTV's own updates: release tag from GitHub's API, version compare, SHA256SUMS lookup (host tests). The thread in `retrotv.c` checks ~90 s after start and daily (option `retrotv_update_check`), and on X > Update or `/update?install=1` downloads the release to `/tmp/retrotv-update/<tag>` and runs its `install.sh` (`RETROTV_BASE=file://…`, `--no-restart`; exit 3 = restart needed) via `systemd-run`. Releases ship `install.sh` (only those can be installed this way). Tests: `RETROTV_UPDATE_CURRENT` pretends a version, `RETROTV_UPDATE_NOINSTALL` stops after the download |
| `src/zapnow.c/.h` | tells Zaparoo Core on the Pi what's on (`media.active.update`, system Video, POST 127.0.0.1:7497/api; loopback needs no pairing): a sender thread with the newest report only, 1 s timeouts; Zaparoo's own tracker doesn't know external cores. Option `retrotv_zaparoo` (default on); `RETROTV_ZAPAROO_PORT` for tests |
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

## Deploying into ReplayOS (its own system — installed on the user's Pi)

RetroTV is a **ReplayOS external system** (ReplayOS 2.1.0+; verified on the Pi 2026-10-31:
system list entry, load_game, sound, System Settings, two screens, last channel).

- **Registration:** `/opt/replay/cores/retrotv_libretro.cfg` next to the core: four quoted fields,
  `id = "retrotv"`, `name = "RetroTV"`, `extensions = "m3u|m3u8|txt"`, `devices = "pi5"`. Unknown or
  repeated fields are rejected (so no icon or direct-launch fields); regular files, no symlinks.
  ReplayOS reads registrations only at frontend start-up (restart RePlay). An external system
  can't pair with a library `cores.cfg` uses. In the menu it's an orange folder: RetroTV, then
  its line-ups. OTA updates keep unowned files in `/opt/replay/cores`, so the install survives
  them (`cores.cfg` itself is restored by updates). Docs: replayos.com/externalcores.
- **Line-ups:** the system's "games" are the files in `roms/retrotv/`. The core takes a launched
  `.m3u`/`.m3u8`, a `.txt` written like playlist.txt (link, `epg=`), or a comment-only `.txt`
  (the installer's `RetroTV.txt`), which falls back to `/media/{nfs,usb,nvme,sd}/retrotv/playlist.txt`
  (first line: URL or path), then `.../retrotv/channels.m3u`. No playlist → setup screen.
- **State:** saves in `<storage>/saves/retrotv/` (`retrotv-last-<playlist hash>.txt`: one per
  line-up), settings in `config/settings/system/<crt|lcd>/retrotv.cfg`. The EPG comes from the
  playlist's `x-tvg-url`, or an `epg=` line (overrides it; `.xml.gz` via the system zlib).
  Matching: tvg-id, then display name, then number (own guide only). Tunarr: use
  `/api/channels.m3u` (guide at `/api/xmltv.xml`), not `/stream/channels/N.m3u8`.
- **Installers:** users run the one-liner (`install.sh` at the repo root, `curl … | bash` on the Pi
  as root). It checks ReplayOS is 2.1.0+, downloads the latest GitHub Release, checks `SHA256SUMS`,
  copies the core to `<storage>/retrotv/` and the Extras scripts (install, uninstall, update) to
  `roms/_extra/`, and runs `replayos/retrotv_install.sh`: core + manifest into `/opt/replay/cores`
  (new file then `mv`, never over the running core), `roms/retrotv/RetroTV.txt`, the remote
  launcher to `/media/sd/retrotv/`, the Zaparoo launcher file, then a ReplayOS restart through
  `systemd-run` (nothing of ours left running on NFS). `--no-restart` (RetroTV's own updater)
  skips the restart and exits 3 when one is still needed. It also moves an install from before
  2.1.0 over (those borrowed the avtest/nintendo_vb/pibench slots): slots get their own cores
  back (backup in `<storage>/retrotv/backup/`), settings and saves are copied. `--slot` is
  accepted and ignored. `retrotv_uninstall.sh` removes the core, manifest and launchers.
  Storage backups use plain `cp` (the NFS share squashes root, so `cp -p` fails).
  `tests/test_install.sh` runs both against a fake ReplayOS in a user namespace (`unshare -r`).
- **Releases:** `.github/workflows/release.yml` on a `v*` tag; bump `RETROTV_VERSION` first. Releases
  ship `install.sh` (RetroTV's in-core updater needs it). Test the installer against a local copy
  of a release with `RETROTV_BASE=file:///dir`.
- **ReplayOS facts:** `replay.cfg` lives in memory and is written on shutdown: never edit it while
  `replay.service` runs (the edit gets overwritten); change settings from its menu. If storage
  silently falls back to `sd`, the systems list empties; reset it in the menu.
  `audio_system_volume = "0"` means volume **Off** (scale 10…0). Core options v2 show in the
  in-game menu as **System Settings** (with a save entry); changes reach the core live.
- **Logs and API:** `/tmp/retrotv.log` on the Pi (the harness writes there too). ReplayOS API on
  55356, header `X-RePlay-Token` (`replay_http_token` in replay.cfg; never print or commit it):
  `get_status` (`core_info`, `system`), `get_info` (display refresh), `load_game?system=retrotv&
  game_file=RetroTV.txt`, `set_cmd?cmd=screenshot` → `<storage>/captures/retrotv/`. There's no
  "quit game" command. Errors and rejected registrations: `/var/log/replay.log`.
- **Remote start:** `replayos/retrotv_launch.sh [channel | retrotv://channel]`, installed to
  **/media/sd/retrotv/** (the SD card): starts RetroTV through load_game if it isn't running, then
  tunes. Lesson: a ReplayOS restart remounts NFS storage, and a script running from `/media/nfs`
  during it keeps the share busy, so ReplayOS falls back to `system_storage = "sd"`.
- **Autostart** (facts verified on 2.0.60): ReplayOS reads a file in `<storage>/roms/_autostart/`
  at start; its content is a path appended to the storage root (starts with `/`), and the core
  comes from the parent folder's system. For RetroTV that's `/roms/retrotv/RetroTV.txt` (not yet
  tried on 2.1). On 2.0.60 an autostarted game with its own Dual horizontal setting segfaulted
  ReplayOS at start-up, so don't test autostart on the user's Pi casually.
- **Zaparoo Core** (user-installed in `/media/sd/zaparoo/`, API on 7497, loopback needs no pairing):
  its launch on ReplayOS first "stops the active launcher" = **restarts replay.service**, so channel
  cards use `**execute:bash /media/sd/retrotv/retrotv_launch.sh <ch>` (the installer adds the
  `allow_execute` rule). `retrotv-zaparoo.toml` registers a RetroTV virtual system and the
  `RetroTVChannel` launcher (`schemes = ["retrotv"]`: Zaparoo history items); Zaparoo rejects
  `name` on non-virtual launchers. Its tracker doesn't know external cores, so RetroTV reports
  what's on itself (`zapnow.c`). Its virtual gamepad is player 2 (RetroTV reads players 1-4), and
  menus need ReplayOS's "all players control the UI".
- Screenshots for the docs: `PI=root@<pi> scripts/demo-shots.sh` (demo line-up, never real channels).
- **Two screens** (verified 2026-10-17 with two CRTs on the Pi's HDMI ports): ReplayOS "Multi screen"
  (`video_monitor_multi_mode`, or per system/game `replay_video_monitor_multi_mode`) = Dual
  horizontal (2) splits one core picture: left half → screen 1, right half → screen 2. Rules learned
  the hard way: both screens connected at boot; **never change geometry or refresh while running**
  (SET_SYSTEM_AV_INFO → page flips fail, main CRT flashes until reboot); Dual smart applies only to
  dual-screen arcade games. Dual modes ReplayOS picks: 1280x480 → `1920x480i` (39.147 MHz, 2496x523)
  on both outputs; 1280x240 → `1920x240p` (39.04 MHz, 2494x261). After any mode change the two outputs
  start with a random phase; out of step, ReplayOS runs at exactly half rate (retro_run every 33 ms).
  A re-sent identical mode is ignored, a 1-frame bounce doesn't fully restart the outputs, and a
  1 s bounce is only a coin toss, so the size is fixed per session and RetroTV just logs it.
  A loose RGB-Pi v2 HDMI-to-SCART adapter caused most of the "lost sync" flicker blamed on modes.
  RetroTV's `retrotv_dual` option (read at load) gives a fixed 1280x480 (or 1280x240 with Output 240p)
  at 60 Hz: the globals are the focused screen, `other` the second; `exchange()` swaps them (see the
  comment in `src/retrotv.c`).

## Roadmap and design

- `docs/ROADMAP.md` — milestones and QoL ideas.
- `docs/DESIGN.md` — the retro cable TV look: guide grid, now-playing banner,
  options. Mockups are linked there.
