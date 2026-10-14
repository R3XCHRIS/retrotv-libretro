# RetroTV (libretro core) — prototype

Flip through live IPTV channels from an M3U playlist like a 90s cable box, as a
libretro core for **ReplayOS on the Raspberry Pi 5**.

- Channel up/down, last channel, info banner, channel list
- Static and hiss while tuning, blue **NO SIGNAL** screen
- Zoom button (FIT/ZOOM) for letterboxed pictures; 50 Hz output on 25/50 fps channels
- Plays live MPEG-TS/HLS over HTTP using ReplayOS's own FFmpeg (nothing to install on the Pi)

> **Status:** early prototype. It runs inside ReplayOS 2.0.60 by borrowing the
> PiBench slot (below). See [CLAUDE.md](CLAUDE.md) for build,
> test and design notes, and [docs/ROADMAP.md](docs/ROADMAP.md) for what's next.

## Controls (prototype)

Button names are the labels on an Xbox-style pad (ReplayOS maps pads by position).

| Button | Action |
|---|---|
| D-pad Up / R | Channel up |
| D-pad Down / L | Channel down |
| A | Show info banner |
| B | Last channel |
| Y | Zoom: letterboxed (FIT) or filled 4:3 (ZOOM) |
| Select | Channel list (Up/Down, A to tune, B to close) |
| Start | TV guide (Up/Down channel, Left/Right 30 min, A to watch, B or Start to close) |

## Install on ReplayOS (borrows the A/V test slot)

ReplayOS can't add new systems yet, so RetroTV takes over **Extra → audio_video_test**
(the `pibench` slot is muted by ReplayOS, so it isn't used).
`<storage>` is your ReplayOS storage (`/media/nfs`, `/media/usb`, `/media/nvme` or `/media/sd`).

1. Copy `retrotv_libretro.so` (CI artifact `retrotv-aarch64`) to `<storage>/retrotv/`.
2. Put your M3U URL (or a path) on the first line of `<storage>/retrotv/playlist.txt`.
3. Copy `replayos/retrotv_install.sh` and `retrotv_uninstall.sh` to `<storage>/roms/_extra/`.
4. Run **Extra → retrotv_install.sh** (or `bash <storage>/roms/_extra/retrotv_install.sh` as root).
   It backs up `cores.cfg`, points `[avtest]` at RetroTV and restarts ReplayOS.
5. Open **Extra → audio_video_test**.

Re-run the installer after each ReplayOS update. `retrotv_uninstall.sh` gives the slot back to the A/V test pattern.

## Build

```bash
pip install ziglang==0.13.0
make deps && make test && make
```

## AI Transparency

This project was developed with significant AI assistance (primarily Claude by
Anthropic). The author reviews, understands, tests, and maintains all code. See
[AI_POLICY.md](AI_POLICY.md) for contribution guidelines.

---

MIT licensed. Third-party code: `include/libretro.h` (MIT, The RetroArch team)
and `src/font8x8_basic.h` (public domain, Daniel Hepper / IBM VGA font). Not
affiliated with ReplayOS, libretro or Dispatcharr.
