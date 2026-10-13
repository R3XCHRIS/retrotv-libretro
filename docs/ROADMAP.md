# Roadmap

## M0 — Prototype (this commit)
- [x] Cross-build for the Pi 5 against ReplayOS's own FFmpeg 7.1 (loaded at runtime with dlopen)
- [x] M3U parser: `#EXTINF` tvg-id/name/logo/chno, group-title, `x-tvg-url`
- [x] Live playback: H.264/AAC MPEG-TS over HTTP, with audio as the master clock
- [x] Channel up/down (D-pad up/down or R/L), last channel (B), info banner (A), channel list (Select)
- [x] Static and hiss while tuning, blue "NO SIGNAL" screen on errors
- [x] Headless harness and Pi test with screenshots

## M1a — Let Dispatcharr do the heavy lifting (next)
Dispatcharr (v0.32.0 here) can already serve a channel subset and pre-transcoded
streams through its normal M3U link. RetroTV keeps reading plain M3U + XMLTV;
no Dispatcharr API client is needed for this.

- [x] **Channel subset:** a Dispatcharr *Channel Profile* (e.g. "RetroTV") with only the
  channels to flip through. Its link is `/output/m3u/<profile>` and the EPG is `/output/epg/<profile>`.
- [x] **Crop/downscale before the Pi:** a Dispatcharr *Output Profile* (Settings → Output
  Profiles; ffmpeg reading `pipe:0`, writing MPEG-TS to `pipe:1`), selected with
  `?output_profile=<id>` on the M3U link. Dispatcharr then appends it to every
  `/proxy/ts/stream/<uuid>` URL. One transcode runs per active channel and is shared by all clients.
  Parameters (dry-run tested from the Mac on live channels; 640x480 SAR 1:1, source fps kept,
  stereo 48 kHz AAC, a keyframe every second so joining a running transcode is quick):
  - **crop** (fills the screen, loses the sides of 16:9):
    `-i pipe:0 -map 0:v:0 -map 0:a:0? -vf "scale='trunc(iw*sar/2)*2':ih,setsar=1,crop='min(iw,ih*4/3)':ih,scale=640:480,setsar=1" -c:v libx264 -preset veryfast -tune zerolatency -force_key_frames "expr:gte(t,n_forced*1)" -b:v 1500k -maxrate 1800k -bufsize 1500k -c:a aac -ac 2 -ar 48000 -b:a 128k -f mpegts pipe:1`
  - **fit** (letterboxes, keeps the whole picture): same, with
    `-vf "scale='trunc(iw*sar/2)*2':ih,setsar=1,scale=640:480:force_original_aspect_ratio=decrease,pad=640:480:(ow-iw)/2:(oh-ih)/2,setsar=1"`
  - The first `scale=...*sar` makes pixels square before cropping. Without it, anamorphic
    sources (seen: 1280x720 with SAR 405:539, a 4:3 picture) get cropped in storage pixels and come out stretched.
  - Sources seen: 1920x1080/1280x720/960x720/640x480/640x464/1920x824, 23.976–50 fps, mostly AC-3 5.1 (one 44.1 kHz).
- [x] `playlist.txt` = `http://<dispatcharr>:9191/output/m3u/<profile>?output_profile=<id>` (deployed Oct 2026 with the VAAPI fit profile)
- [x] Core: fast path when a frame is already 640x480 square-pixel (no resize, no border clear; the
  YUV->RGB conversion still runs because decoders output YUV)
- [x] Measure on the Pi, direct versus transcoded (Oct 2026, below)
- [x] Hardware encode on Yoda (VAAPI): now the default profile, see the measurements below.
  Don't use `-fflags +discardcorrupt` (it starves the VAAPI decoder).
- [x] ~~Let the output profile set the frame rate~~ — not needed: the core now switches the output to 50 Hz itself

### M1a measurements (Pi 5, harness, Oct 2026)

Same 5 channels and 130 s button script for both runs (`scripts/pi-test.sh`, CPU from
`scripts/cpu-summary.py`, % of one core, the Pi 5 has 400%). Sources: 640x480p24 AAC,
1080p25 AC-3 5.1, 1280x720p24 anamorphic AC-3, 1080p50 AC-3, 720p25 AC-3. Transcoded = output profile "fit" (libx264).

| | Direct | Fit, libx264 | Fit, VAAPI (default) |
|---|---|---|---|
| Tune to picture, first visit | 2.8 / 4.0 / 5.2 / 14.6 s, one >16 s | 3.2 / 4.0 / 7.4 / 11.3 s, one >16 s | 3.3 / 4.2 / 4.8 / 12.0 / 13.9 s |
| Tune to picture, revisit after 10–20 s | 4.0–4.3 s | 4.0–4.9 s | 4.0–5.3 s |
| `retro_run` worst per second (median / p95 / max) | 1.7 / 3.7 / 23 ms | 1.5 / 2.4 / 21 ms | 1.8 / 2.5 / 2.7 ms |
| Process CPU (avg / peak) | 26.6% / 78% | **10.8% / 22%** | **11.4% / 22%** |
| `retrotv-dec` + FFmpeg decoder threads (avg) | 6.6% + 12.0% | **1.1% + 1.9%** | **1.2% + 2.4%** |
| Peak threads | 7 | 7 | 7 |
| A/V skew (avg / max) | 4.6–6.2 / 12 ms | 4.4–5.3 / 12 ms | 4.4–5.6 / 12 ms |
| Dropped frames | 0 | 0 | 0 |
| Yoda CPU per transcode (% of one N100 core) | – | 30–60% SD/720p, 130–190% 1080p50 | 20–40% SD/720p, 33–59% 1080p50 |

- Tune time is dominated by Dispatcharr's upstream connection (2.7–13 s cold, variable run to
  run); the transcode adds about 0.1–0.7 s, and its first keyframe arrives with its first byte.
  The ">16 s" channel stalled upstream in both runs (in the transcoded run its ffmpeg sat at 0% CPU).
- There is no "warm" tune here: Dispatcharr's `channel_shutdown_delay` is 0 (default), so a channel stops
  when RetroTV leaves it. A second profile on a still-running channel started in 0.17 s.
- The banner and log show `640x480 h264 ... aac, native` for every transcoded channel (fast path on).
- Cost moves to Yoda (Intel N100, 4 cores): one libx264 transcode is 30–60% of a core for SD/720p and
  130–190% for 1080p50. A full-GPU VAAPI pipeline on the same 1080p50 channel used **3.5x less CPU**
  (7.5 s vs 26.7 s CPU for 22 s of stream) with the same geometry:
  `-hwaccel vaapi -hwaccel_device /dev/dri/renderD128 -hwaccel_output_format vaapi -i pipe:0 -map 0:v:0 -map 0:a:0? -vf "scale_vaapi=w='trunc(iw*sar/2)*2':h=ih,scale_vaapi=w=640:h=480:force_original_aspect_ratio=decrease,pad_vaapi=w=640:h=480:x=(ow-iw)/2:y=(oh-ih)/2,setsar=1" -c:v h264_vaapi -rc_mode VBR -b:v 1500k -maxrate 1800k -bf 0 -force_key_frames "expr:gte(t,n_forced*1)" -c:a aac -ac 2 -ar 48000 -b:a 128k -f mpegts pipe:1`
  (the Dispatcharr container already has `/dev/dri` and FFmpeg 8.1 with `h264_vaapi`/`pad_vaapi`).

## Later — Dispatcharr API (only where M3U/XMLTV can't help)
- [ ] When a channel stays stuck after retries, ask Dispatcharr to reset it
  (`POST /proxy/ts/stop/<uuid>`; its `tried_stream_ids` never clears mid-session).
  Needs an API token stored on the Pi; never commit it.
- [ ] Optional: live stream stats (resolution, bitrate, failover) in the info banner

## M1 — Feels like cable
- [ ] **TV guide** (Select long-press or Start): XMLTV from the playlist's `x-tvg-url`
  (Dispatcharr `/output/epg/...`), a 90-minute grid, scrolling listings
- [ ] **Now / Next** in the banner from the EPG, with a progress bar for the current show
- [ ] **Number entry**: type a channel number with the D-pad ("12_" → tune after 1.5 s)
- [ ] Tune timeout with "retrying", and automatically skip channels that keep failing (configurable)
- [ ] **Favourites** (Start + A) and a favourites-only flip mode
- [ ] Remember the last channel per playlist (saved in the core's save directory)
- [ ] Channel logos in the banner (fetched and cached as PNG/JPEG, decoded with FFmpeg)

## M2 — Options and QoL (libretro core options)
- [x] Zoom button (pad Y): FIT letterboxes, ZOOM crops the picture inside detected bars to fill 4:3
- [x] Loudness: +12 dB with a soft limiter, so TV audio is as loud as games
- [ ] Remember zoom per channel; loudness as an option
- [ ] Output: 4:3 (CRT) or 16:9, overscan-safe margins, resolution 640x480 / 320x240 (240p)
- [x] Refresh follows the stream: 50 Hz for 25/50 fps channels, else 60 Hz (`SET_SYSTEM_AV_INFO`; ReplayOS switches the CRT mode)
- [ ] Option to pin 60 Hz (or 59.94) for displays that don't like mode changes
- [ ] Static style: off / classic / VHS; channel-change "blip" sound on or off
- [ ] Banner timeout, clock 12h/24h, colour theme (Cable Navy, Teletext, Green Phosphor)
- [ ] Sleep timer ("SLEEP 30")
- [ ] Parental lock per group-title (PIN entered with the D-pad)
- [ ] Audio track and subtitle selection (A long-press)
- [ ] Preview tile in the guide (live picture of the highlighted channel)

## Known issues (from Pi testing)
- [ ] Channel change blocks `retro_run` while the old stream closes (up to ~70 ms seen): close on a background thread
- [ ] Tuning takes 4–7 s on cold Dispatcharr channels; a warm channel can start in under 1 s
- [ ] Dispatcharr sometimes fails a cold tune with "Invalid data found when processing input"; retries cover it, but the root cause is unknown

## M3 — Into ReplayOS
- [x] Interim path: borrow a slot. `pibench` turned out to be muted by ReplayOS; now `avtest`
      (Extra → audio_video_test), with sound, verified on 2.0.60
- [x] Installer and uninstaller with backups (`replayos/`)
- [ ] Ask the ReplayOS author for an official user-core hook
- [ ] Check what a ReplayOS update does to the install (expect: re-run the installer)
- [ ] Optional: hardware HEVC decode via V4L2 stateless (the Pi 5 only has HEVC hardware decode)
