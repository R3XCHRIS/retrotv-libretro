#!/usr/bin/env bash
# RetroTV for ReplayOS - update to the latest release (needs internet).
# Runs the one-line installer from the RetroTV repository; your playlist.txt
# and settings are kept. ReplayOS restarts when it's done.
set -eu          # POSIX sh too: people run it as "sh retrotv_update.sh"
if [ "${1:-}" = "NULL" ]; then shift; fi
INSTALLER="https://raw.githubusercontent.com/chris-skitch/retrotv-libretro/main/install.sh"
TMP="/dev/shm/retrotv_install.sh"
curl -fsSL --connect-timeout 10 --retry 3 -o "$TMP" "$INSTALLER" || { echo "Internet required" >&2; exit 1; }
exec bash "$TMP" "$@"
