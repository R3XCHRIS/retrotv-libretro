# Design — retro cable TV look

The target feel is a 1990s cable box on a CRT: a navy banner with a gold rule,
chunky bitmap type, channel numbers in big yellow digits, a scrolling guide
grid, static between channels, and a blue NO SIGNAL screen.

## Constraints

- 640x480 at 4:3, readable on a 14" CRT: text at least 16 px tall (8x8 font at 2x),
  important text kept inside a 5% overscan-safe margin.
- 8x8 bitmap font (ASCII only). Emoji in group titles are stripped.
- Overlays are drawn in software over the video; keep them as solid or
  semi-transparent rectangles.

## Screens

| Screen | Prototype | Notes |
|---|---|---|
| Channel banner (now playing) | ✅ in M0 | number, name, group, clock, tuning or codec line, key hints |
| Channel list | ✅ in M0 | 12 rows, current channel in yellow |
| Tuning static | ✅ in M0 | grey noise with banding, quiet hiss |
| NO SIGNAL | ✅ in M0 | classic blue screen |
| TV guide grid | mockup | needs XMLTV from the playlist's `x-tvg-url` (M1) |
| Now / Next banner with progress | mockup | needs EPG (M1) |
| Options menu | mockup | maps to libretro core options (M2) |

Mockups: see the link in the repo description, or ask the maintainer.
