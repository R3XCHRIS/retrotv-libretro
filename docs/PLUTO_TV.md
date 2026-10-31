# Free channels: Pluto TV

<table align="right">
  <tr><th colspan="2"><a href="https://pluto.tv"><img src="https://img.shields.io/badge/Pluto%20TV-free%20live%20channels-FFF200?style=for-the-badge&labelColor=000000" alt="Pluto TV: free live channels"></a></th></tr>
  <tr><td><b>Cost</b></td><td>Free, with ads</td></tr>
  <tr><td><b>Regions</b></td><td>Several: pick the list for yours</td></tr>
  <tr><td><b>Channels</b></td><td>183 in the UK list</td></tr>
  <tr><td><b>TV guide</b></td><td>Comes with the playlist</td></tr>
  <tr><td><b>Best with</b></td><td>One Pluto channel at a time</td></tr>
  <tr><td><b>Playlists</b></td><td><a href="https://github.com/BuddyChewChew/pluto">BuddyChewChew/pluto</a><br>(unofficial)</td></tr>
</table>

[Pluto TV](https://pluto.tv) is a free, ad-supported streaming service with live channels:
films, cartoons, true crime, sport, documentaries and more.
[BuddyChewChew/pluto](https://github.com/BuddyChewChew/pluto) on GitHub publishes ready-made
M3U playlists of Pluto's channels for several regions, each with a TV guide, and RetroTV plays
them directly.

## Add it as a line-up

1. On [BuddyChewChew/pluto](https://github.com/BuddyChewChew/pluto), open the `.m3u` playlist
   for your region and copy its **Raw** link.
2. Create a text file in `roms/retrotv/` on your ReplayOS storage, named after the line-up
   (e.g. `Pluto TV UK.txt`), with that link in it. For the UK:

   ```
   https://raw.githubusercontent.com/BuddyChewChew/pluto/refs/heads/main/pluto_gb.m3u
   ```

3. Open **RetroTV** in the system list and pick your new line-up.

Channels are numbered in playlist order and grouped by Pluto's categories, with Now/Next and
the TV guide. Your other line-ups stay as they are, and each one remembers its own last channel.

## Good to know

- **Unofficial lists:** these aren't from Pluto TV, so channels come and go, and a link stops
  working if a file is renamed or moved. RetroTV's setup screen then says what went wrong.
- **One Pluto channel at a time works best.** These lists give every channel, for everyone who
  uses them, the same device and session IDs, and Pluto ends streams it sees as duplicates.
  With [two screens](../README.md#two-screens) both on Pluto, they knock each other out: in tests,
  each screen dropped about every 20 seconds, against none to a few drops in 5 minutes with one
  Pluto channel. Pluto on one screen and your other channels on the other is fine.
- **Drop-outs:** when Pluto ends a stream, RetroTV reconnects by itself, holding the picture for
  a few seconds.
- **Region:** pick the list for where you are. Channels from another region's list may not play.
- **Channel numbers:** each line-up numbers its own channels, so a Zaparoo channel card tunes
  whichever line-up is playing.

---

Pluto TV is a trademark of Pluto Inc. RetroTV isn't affiliated with Pluto TV or with the
playlists' author.
