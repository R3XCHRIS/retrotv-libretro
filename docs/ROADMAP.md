# Roadmap

## M0 — Prototype (this commit)
- [x] Cross-build for the Pi 5 against ReplayOS's own FFmpeg 7.1 (loaded at runtime with dlopen)
- [x] M3U parser: `#EXTINF` tvg-id/name/logo/chno, group-title, `x-tvg-url`
- [x] Live playback: H.264/AAC MPEG-TS over HTTP, with audio as the master clock
- [x] Channel up/down (D-pad up/down or R/L), last channel (B), info banner (A), channel list (Select)
- [x] Static and hiss while tuning, blue "NO SIGNAL" screen on errors
- [x] Headless harness and Pi test with screenshots

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
- [ ] Output: 4:3 (CRT) or 16:9, overscan-safe margins, resolution 640x480 / 320x240 (240p)
- [ ] Refresh: 60 / 59.94 / 50 Hz, ideally following the stream (e.g. a 50 fps PAL channel judders at 60 Hz)
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
- [x] Interim path: borrow the `pibench` slot (Extra → pibench), verified on 2.0.60
- [x] Installer and uninstaller with backups (`replayos/`)
- [ ] Ask the ReplayOS author for an official user-core hook
- [ ] Check what a ReplayOS update does to the install (expect: re-run the installer)
- [ ] Optional: hardware HEVC decode via V4L2 stateless (the Pi 5 only has HEVC hardware decode)
