#include "onboard.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static int has(const char *s, const char *needle) { return s && strcasestr(s, needle) != NULL; }

void onboard_explain(ob_kind kind, const char *err, char *problem, size_t pn, char *hint, size_t hn)
{
  const char *p = "Playlist problem", *h = "Check the link in retrotv/playlist.txt.";
  switch (kind) {
  case OB_NO_FILE:
    p = "No playlist yet";
    h = "Create retrotv/playlist.txt on your ReplayOS storage with your M3U link on the first line.";
    break;
  case OB_EMPTY_FILE:
    p = "playlist.txt is empty";
    h = "Put your M3U link on the first line that doesn't start with #.";
    break;
  case OB_NOT_M3U:
    p = "That link isn't a playlist";
    h = "It returned something else (a web page?). Use the M3U link, e.g. Dispatcharr's /output/m3u/...";
    break;
  case OB_NO_CHANNELS:
    p = "The playlist has no channels";
    h = "Add channels to it (in Dispatcharr: add channels to the channel profile in the link).";
    break;
  case OB_FETCH:
    if (has(err, "Protocol not found") || has(err, "Invalid argument")) {
      p = "That isn't a link or a path";
      h = "The first line must start with http://, https:// or / (a file on the Pi).";
    } else if (has(err, "404") || has(err, "returned 404")) {
      p = "Playlist not found (404)";
      h = "The server answered but the path is wrong: check the channel profile name in the link.";
    } else if (has(err, "401") || has(err, "403") || has(err, "Unauthorized") || has(err, "Forbidden")) {
      p = "Access denied by the server";
      h = "Allow this Pi's address for M3U/EPG in the server's network access settings, or add credentials.";
    } else if (has(err, "refused")) {
      p = "Server refused the connection";
      h = "Is the server running? Check the address and port in the link (Dispatcharr uses 9191).";
    } else if (has(err, "timed out") || has(err, "timeout") || has(err, "unreachable")) {
      p = "Server didn't answer";
      h = "Check the address in the link and that the Pi and the server are on the same network.";
    } else if (has(err, "resolve") || has(err, "name or service") || has(err, "Name or service")) {
      p = "Unknown server name";
      h = "The name in the link can't be found: try the server's IP address instead.";
    } else if (has(err, "No such file") || has(err, "cannot read")) {
      p = "Playlist file not found";
      h = "The path in playlist.txt doesn't exist on the Pi. Use a full path or a link.";
    } else {
      p = "Couldn't load the playlist";
      h = err && *err ? err : "Check the link in retrotv/playlist.txt.";
    }
    break;
  }
  snprintf(problem, pn, "%s", p);
  snprintf(hint, hn, "%s", h);
}

int onboard_is_m3u(const char *text)
{
  if (!text) return 0;
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n' || (unsigned char)*text == 0xEF
         || (unsigned char)*text == 0xBB || (unsigned char)*text == 0xBF) text++;   /* BOM, blanks */
  if (!strncasecmp(text, "#EXTM3U", 7) || strstr(text, "#EXTINF")) return 1;
  for (const char *l = text; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL)  /* plain list of URLs */
    if (!strncmp(l, "http://", 7) || !strncmp(l, "https://", 8) || !strncmp(l, "udp://", 6) || !strncmp(l, "rtp://", 6)) return 1;
  return 0;
}
