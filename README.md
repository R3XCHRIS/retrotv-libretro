# RetroTV (libretro core) — prototype

Flip through live IPTV channels from an M3U playlist like a 90s cable box, as a
libretro core for **ReplayOS on the Raspberry Pi 5**.

- Channel up/down, last channel, info banner, channel list
- Static and hiss while tuning, blue **NO SIGNAL** screen
- Plays live MPEG-TS/HLS over HTTP using ReplayOS's own FFmpeg (nothing to install on the Pi)

> **Status:** early prototype. It's tested headlessly on a real Pi 5 and is
> **not yet installed into ReplayOS**. See [CLAUDE.md](CLAUDE.md) for build,
> test and design notes, and [docs/ROADMAP.md](docs/ROADMAP.md) for what's next.

## Controls (prototype)

| Button | Action |
|---|---|
| D-pad Up / R | Channel up |
| D-pad Down / L | Channel down |
| B | Last channel |
| A | Show info banner |
| Select | Channel list (Up/Down, A to tune, B to close) |

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
