#!/usr/bin/env bash
# RetroTV for ReplayOS - install into the "avtest" slot (Extra > audio_video_test).
#
# Put this file in <storage>/roms/_extra/ and run it from the ReplayOS
# Extra menu (or over SSH as root). Re-run it after every ReplayOS update:
# updates replace /opt/replay, which undoes the install.
#
# Needs <storage>/retrotv/retrotv_libretro.so (storage = the ReplayOS storage
# root: /media/nfs, /media/usb, /media/nvme or /media/sd).
#
# What it changes (with backups):
#   /opt/replay/cores/retrotv_libretro.so      added
#   /opt/replay/cores/cores.cfg [avtest]       low/mid/hi -> retrotv_libretro.so
#   /opt/replay/cores/cores.cfg [pibench]      back to pibench_libretro.so if an
#                                              older RetroTV install had taken it
# avtest_libretro.so itself is left untouched; the uninstaller restores the slot.
# Why avtest: ReplayOS mutes the pibench slot (no audio there), and it can't
# add new systems, so RetroTV borrows the A/V test pattern's slot.
set -euo pipefail
[[ "${1:-}" == "NULL" ]] && shift   # ReplayOS passes NULL when launched from the menu

CORES=/opt/replay/cores
CFG=$CORES/cores.cfg
SLOT=avtest
NOW=$(date +%Y%m%d-%H%M%S)

say() { echo "[retrotv] $*"; }
die() { echo "[retrotv] ERROR: $*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || die "run as root"
[[ -f $CFG ]] || die "$CFG not found - is this ReplayOS?"
grep -q "^\[$SLOT\]" "$CFG" || die "no [$SLOT] section in $CFG - ReplayOS changed; not installing"

# storage root: this script lives in <storage>/roms/_extra, else search the usual mounts
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
STORAGE=""
[[ -f "$HERE/../../retrotv/retrotv_libretro.so" ]] && STORAGE=$(cd "$HERE/../.." && pwd)
if [[ -z $STORAGE ]]; then
  for r in /media/nfs /media/usb /media/nvme /media/sd; do
    [[ -f "$r/retrotv/retrotv_libretro.so" ]] && { STORAGE=$r; break; }
  done
fi
[[ -n $STORAGE ]] || die "retrotv/retrotv_libretro.so not found on any storage"
SRC=$STORAGE/retrotv/retrotv_libretro.so
say "core: $SRC"

mkdir -p "$STORAGE/retrotv/backup"
# plain cp: NFS shares usually squash root, so ownership can't be preserved there
[[ -f "$STORAGE/retrotv/backup/cores.cfg.original" ]] || cp "$CFG" "$STORAGE/retrotv/backup/cores.cfg.original"
cp "$CFG" "$STORAGE/retrotv/backup/cores.cfg.$NOW"
[[ -f "$CFG.pre-retrotv" ]] || cp -p "$CFG" "$CFG.pre-retrotv"
say "backed up cores.cfg"

install -m 0644 "$SRC" "$CORES/retrotv_libretro.so.new"
mv -f "$CORES/retrotv_libretro.so.new" "$CORES/retrotv_libretro.so"
say "installed $CORES/retrotv_libretro.so"

# point a slot's low/mid/hi at a core
set_slot() {
  local tmp
  tmp=$(mktemp)
  awk -v slot="[$1]" -v core="$2" '
    /^\[/ { insec = ($0 == slot) }
    insec && /^(low|mid|hi)[[:space:]]*=/ { split($0, kv, "="); gsub(/[[:space:]]/, "", kv[1]); print kv[1] " = \"" core "\""; next }
    { print }' "$CFG" >"$tmp"
  cat "$tmp" >"$CFG"
  rm -f "$tmp"
}

if awk '/^\[/{insec=($0=="[pibench]")} insec && /retrotv_libretro\.so/' "$CFG" | grep -q .; then
  set_slot pibench pibench_libretro.so
  say "[pibench] given back to PiBench (older RetroTV install)"
fi
set_slot "$SLOT" retrotv_libretro.so
n=$(awk -v slot="[$SLOT]" '/^\[/{insec=($0==slot)} insec && /retrotv_libretro\.so/' "$CFG" | wc -l)
[[ $n -eq 3 ]] || { cat "$STORAGE/retrotv/backup/cores.cfg.$NOW" >"$CFG"; die "cores.cfg edit failed - restored backup"; }
say "[$SLOT] now launches RetroTV"

if [[ ! -f "$STORAGE/retrotv/playlist.txt" ]]; then
  cat >"$STORAGE/retrotv/playlist.txt" <<'TEMPLATE'
# RetroTV playlist
#
# CHANNELS: the first line that doesn't start with # is your channel list,
# either an M3U link (http://...) or the full path to an .m3u file.
#   Dispatcharr:  http://<server>:9191/output/m3u/<channel-profile>
#   Tunarr:       http://<server>:8088/api/channels.m3u
#
# TV GUIDE: taken from the playlist automatically (its x-tvg-url).
# To use a different guide, or if your playlist has none, remove the #
# in front of the epg= line below and put in an XMLTV link or path
# (.xml or .xml.gz):
#   epg=http://<server>/guide.xml.gz
#
# Lines starting with # are ignored. Close and reopen RetroTV after a change.
# Put your channel list link on the next line:

TEMPLATE
  say "created $STORAGE/retrotv/playlist.txt - put your M3U link in it"
fi
say "done - restarting ReplayOS in 3 s. Then open Extra > audio_video_test."
( sleep 3; systemctl restart replay.service ) >/dev/null 2>&1 &
