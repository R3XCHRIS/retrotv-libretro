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

| Screen | Status | Notes | Screenshot |
|---|---|---|---|
| Channel banner with Now / Next | ✅ M0, M1 | number, name, group, clock, NOW with progress bar, NEXT, status line, hints | [banner](screenshots/banner.png) |
| Channel list | ✅ M0 | 12 rows, current channel highlighted | [list](screenshots/channel-list.png) |
| Tuning static | ✅ M0, M2 | classic snow with banding, or VHS (tracking band, colour speckle) | [snow](screenshots/tuning.png), [VHS](screenshots/tuning-vhs.png) |
| NO SIGNAL | ✅ M0 | classic blue screen with the reason on the banner | [no signal](screenshots/no-signal.png) |
| TV guide grid | ✅ M1 | Start; live preview, info pane, 7 rows x 90 min, red "now" line | [guide](screenshots/guide.png) |
| Quick menu (X) | ✅ M2 | session changes: sleep, audio, leveller, picture, output, test card, controls | [menu](screenshots/menu.png) |
| Settings | ✅ M2 | libretro core options; ReplayOS shows them as System Settings | |
| Themes | ✅ M2 | Cable Navy, Teletext, Green Phosphor | [teletext](screenshots/theme-teletext.png), [phosphor](screenshots/theme-phosphor.png), [teletext guide](screenshots/guide-teletext.png) |
| Setup screen, test card, welcome | ✅ M2 | onboarding: the problem in plain words, bars and tone, controls | [setup](screenshots/setup.png), [test card](screenshots/test-card.png), [welcome](screenshots/welcome.png) |
| 240p | ✅ | menus drawn at 240 lines, 1x text keeps 8 rows | [banner](screenshots/banner-240p.png), [guide](screenshots/guide-240p.png) |

Screenshots come from the demo line-up (`tests/demo/make-demo.sh`, made-up channels,
FFmpeg-generated video) via `scripts/demo-shots.sh`, never from real channels.

Mockups (private Claude design canvas, ask the maintainer for access):
https://claude.ai/artifact/Lny4q3dTsFAPygoMHzHVbn

It has four 4:3 screens (TV guide, Now Playing, channel change, Options), each with a
Theme tweak (Cable Navy / Teletext / Green Phosphor) and a scanlines toggle.
