# AI Policy

## For users

This project is built with a lot of help from AI, mainly Claude by Anthropic.
That covers the C code, the tests and the documentation.

The maintainer reviews, understands, tests and maintains all of the code.
Changes are tested on a real Raspberry Pi 5 running ReplayOS, using a headless
test frontend with screenshots that are checked by eye, as well as with unit
tests in CI. The core doesn't modify ReplayOS. Installing it there is a separate,
manual step that's documented with backups.

If something doesn't behave as described, please
[open an issue](https://github.com/R3XCHRIS/retrotv-libretro/issues).

## For contributors

AI-assisted contributions are welcome, on these conditions:

1. **Understand what you submit.** You must be able to explain every change in
   your pull request without going back to the AI tool.
2. **Say so.** Mention in the pull request description which AI tools you used
   and what for.
3. **Test it.** `make test` and `make check` must pass. For playback changes,
   run `make pi-test` on real hardware and attach a screenshot.
4. **A human writes or reviews the words.** Comments, docs and pull request text
   must be written or properly reviewed by a person.
5. **Keep the core rules.** `retro_run` never blocks. Never commit stream URLs,
   playlists or tokens.

Pull requests that don't meet these may be closed without detailed review.

## Inspiration

Adapted in spirit from the AI policy of
[replay-control](https://github.com/lapastillaroja/replay-control/blob/main/AI_POLICY.md),
which credits the EFF's policy on LLM-assisted contributions and Ghostty's AI policy.
