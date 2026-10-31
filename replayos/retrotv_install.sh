#!/usr/bin/env bash
# RetroTV for ReplayOS 2.1.0 or newer - install RetroTV as its own system:
# "RetroTV" in the system list, with your line-ups as its games (roms/retrotv/*.txt, *.m3u).
#
#   retrotv_install.sh [--no-restart]
#
#   --no-restart  don't restart ReplayOS (RetroTV's own updater): the new core is used
#           when RetroTV is next opened. Exits 3 if a restart is still needed because
#           the system list changes (cores.cfg or the manifest).
#
# Put this file in <storage>/roms/_extra/ and run it from the ReplayOS Extra menu (or
# over SSH as root). Needs <storage>/retrotv/retrotv_libretro.so (storage = the ReplayOS
# storage root: /media/nfs, /media/usb, /media/nvme or /media/sd). ReplayOS updates keep
# the install (they leave unowned files in /opt/replay/cores alone).
#
# What it changes:
#   /opt/replay/cores/retrotv_libretro.so    the core
#   /opt/replay/cores/retrotv_libretro.cfg   the manifest that makes it a system (read at start-up)
#   <storage>/roms/retrotv/RetroTV.txt       a line-up using retrotv/playlist.txt (when the
#                                            folder has no line-ups yet)
#   /media/sd/retrotv/retrotv_launch.sh      remote start (load_game), for scripts and Zaparoo
#   /media/sd/zaparoo/launchers/retrotv.toml with Zaparoo: RetroTV, channel cards, history
# Older RetroTV versions borrowed ReplayOS's A/V test, Virtual Boy or PiBench slot (before
# 2.1.0 ReplayOS couldn't add systems). An install from then is moved over: those slots get
# their own cores back (backup of cores.cfg in <storage>/retrotv/backup/), and RetroTV's
# settings and saves are copied to the RetroTV system.
set -euo pipefail
[[ "${1:-}" == "NULL" ]] && shift   # ReplayOS passes NULL when launched from the menu

CORES=${RETROTV_CORES:-/opt/replay/cores}            # tests point these at a scratch tree
SD=${RETROTV_SD:-/media/sd}
CFG=$CORES/cores.cfg
MANIFEST=$CORES/retrotv_libretro.cfg
NOW=$(date +%Y%m%d-%H%M%S)
NORESTART=0

say() { echo "[retrotv] $*"; }
die() { echo "[retrotv] ERROR: $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-restart) NORESTART=1 ;;
    --slot) [[ $# -ge 2 ]] && shift; say "--slot is no longer needed: RetroTV is always its own system" ;;
    NULL) ;;
    *) die "unknown option: $1" ;;
  esac
  shift
done

replay_version() { dpkg-query -W -f='${Version}' replay-frontend 2>/dev/null || true; }
# restart ReplayOS from a systemd timer: nothing of ours is left running on the
# storage when ReplayOS remounts it (a busy NFS share pushes it onto the SD card)
restart_replay() {
  cd /
  systemd-run --quiet --on-active=3 --unit="retrotv-restart-$(date +%s)" systemctl restart replay.service 2>/dev/null ||
    ( sleep 3; systemctl restart replay.service ) </dev/null >/dev/null 2>&1 &
}

[[ $EUID -eq 0 ]] || die "run as root"
[[ -f $CFG ]] || die "$CFG not found - is this ReplayOS?"
v=$(replay_version)
[[ -n $v && $(printf '%s\n' 2.1.0 "$v" | sort -V | head -1) == 2.1.0 ]] ||
  die "RetroTV needs ReplayOS 2.1.0 or newer (this is ${v:-unknown}): update ReplayOS first"

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

# what ReplayOS reads at start-up: if neither changes, no restart is needed
startup_files() { { cat "$CFG"; cat "$MANIFEST" 2>/dev/null || true; } | md5sum; }
before=$(startup_files)

install -m 0644 "$SRC" "$CORES/retrotv_libretro.so.new"
mv -f "$CORES/retrotv_libretro.so.new" "$CORES/retrotv_libretro.so"
say "installed $CORES/retrotv_libretro.so"

# the manifest ReplayOS reads at start-up, next to the core
printf '%s\n' '# RetroTV as its own system (ReplayOS 2.1.0+). Written by retrotv_install.sh.' \
  'id = "retrotv"' 'name = "RetroTV"' 'extensions = "m3u|m3u8|txt"' 'devices = "pi5"' >"$MANIFEST.new"
chmod 0644 "$MANIFEST.new"
mv -f "$MANIFEST.new" "$MANIFEST"
SYSDIR=$STORAGE/roms/retrotv
mkdir -p "$SYSDIR"
if ! find "$SYSDIR" -maxdepth 1 -type f \( -iname '*.txt' -o -iname '*.m3u' -o -iname '*.m3u8' \) ! -name '.*' | grep -q .; then
  printf '%s\n' '# RetroTV: open this to watch the channels in <storage>/retrotv/playlist.txt.' \
    '# To make it a line-up of its own, put an M3U link (or a path) on a line below,' \
    '# and optionally epg=<guide link>. Each .txt or .m3u file here is a line-up.' >"$SYSDIR/RetroTV.txt"
  say "added $SYSDIR/RetroTV.txt (uses retrotv/playlist.txt)"
fi
say "registered RetroTV as a system ($MANIFEST)"

# An install from before 2.1.0 borrowed a slot: give it back (an external system
# can't share its core with cores.cfg), from cores.cfg as it was before RetroTV
own_core() {
  local c=""
  [[ -f $STORAGE/retrotv/backup/cores.cfg.original ]] &&
    c=$(awk -v slot="[$1]" '/^\[/{insec=($0==slot)} insec && /^low[[:space:]]*=/{gsub(/.*= *"|".*/, ""); print; exit}' "$STORAGE/retrotv/backup/cores.cfg.original")
  [[ -n $c && $c != retrotv_libretro.so ]] && echo "$c" || echo "$2"
}
has_retrotv() { awk -v slot="[$1]" '/^\[/{insec=($0==slot)} insec && /retrotv_libretro\.so/' "$CFG" | grep -c . || true; }
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
if grep -q retrotv_libretro "$CFG"; then
  cp "$CFG" "$STORAGE/retrotv/backup/cores.cfg.$NOW"           # plain cp: NFS squashes root
  for s in avtest:avtest_libretro.so nintendo_vb:mednafen_vb_libretro.so pibench:pibench_libretro.so; do
    sec=${s%%:*}
    if [[ $(has_retrotv "$sec") -gt 0 ]]; then
      set_slot "$sec" "$(own_core "$sec" "${s#*:}")"
      say "[$sec] given back to its own core (older RetroTV install)"
    fi
  done
  ! grep -q retrotv_libretro "$CFG" || die "cores.cfg still mentions RetroTV - check $CFG (backup in $STORAGE/retrotv/backup/)"
fi
rm -f "$STORAGE/roms/nintendo_vb/RetroTV.vb" "$STORAGE/retrotv/slot.txt"

# RetroTV's settings and saves are per system: a first install of the system starts with
# a copy of an old slot's (System Settings, two screens, last channel, zoom)
seed() {   # from
  local d
  for d in "$STORAGE"/config/settings/system/*; do
    [[ -f $d/$1.cfg && ! -f $d/retrotv.cfg ]] && cp "$d/$1.cfg" "$d/retrotv.cfg" && say "settings copied: $(basename "$d")/$1.cfg -> retrotv.cfg"
  done
  if ls "$STORAGE/saves/$1"/retrotv-* >/dev/null 2>&1 && ! ls "$STORAGE/saves/retrotv"/retrotv-* >/dev/null 2>&1; then
    mkdir -p "$STORAGE/saves/retrotv" && cp "$STORAGE/saves/$1"/retrotv-* "$STORAGE/saves/retrotv/" && say "saves copied: $1 -> retrotv"
  fi
  return 0                                         # nothing to copy is fine (set -e)
}
seed nintendo_vb                                   # first: the slot two-screen setups used
seed avtest

# Remote start: the launcher lives on the SD card, because a ReplayOS restart
# remounts NFS storage and a script running from there blocks it
LAUNCH=$SD/retrotv/retrotv_launch.sh
if [[ -f $STORAGE/retrotv/retrotv_launch.sh ]]; then
  mkdir -p "$SD/retrotv"
  cp "$STORAGE/retrotv/retrotv_launch.sh" "$LAUNCH" && chmod 755 "$LAUNCH"
  say "remote start: bash $LAUNCH [channel]"
fi

# Zaparoo (optional): RetroTV as a system, channel cards (**execute) and history items
ZDIR=$SD/zaparoo
ZRULE="allow_execute = ['^bash /media/sd/retrotv/retrotv_launch\\.sh( [A-Za-z0-9._-]+)?\$']"
if [[ -d $ZDIR/launchers && -f $STORAGE/retrotv/retrotv-zaparoo.toml ]]; then
  cp "$STORAGE/retrotv/retrotv-zaparoo.toml" "$ZDIR/launchers/retrotv.toml"
  if [[ -f $ZDIR/config.toml ]] && ! grep -q 'retrotv_launch' "$ZDIR/config.toml"; then
    cp "$ZDIR/config.toml" "$ZDIR/config.toml.before-retrotv"
    if ! grep -q '^\[zapscript\]' "$ZDIR/config.toml"; then
      printf '\n[zapscript]\n%s\n' "$ZRULE" >>"$ZDIR/config.toml"
    elif ! grep -q '^allow_execute' "$ZDIR/config.toml"; then
      awk -v rule="$ZRULE" '{ print } /^\[zapscript\]/ { print rule }' "$ZDIR/config.toml" >"$ZDIR/config.toml.new" &&
        cat "$ZDIR/config.toml.new" >"$ZDIR/config.toml" && rm -f "$ZDIR/config.toml.new"
    else
      say "Zaparoo: add this to allow_execute in $ZDIR/config.toml for channel cards:"
      say "  '^bash /media/sd/retrotv/retrotv_launch\\.sh( [A-Za-z0-9._-]+)?\$'"
    fi
  fi
  systemctl try-restart zaparoo.service >/dev/null 2>&1 || true
  say "Zaparoo: RetroTV is a system there; channel cards: **execute:bash $LAUNCH <channel>"
fi

if [[ $NORESTART -eq 1 ]]; then
  if [[ $(startup_files) != "$before" ]]; then
    say "done - restart ReplayOS to finish (the system list changed). Then open RetroTV in the system list."
    exit 3
  fi
  say "done - the new version starts the next time you open RetroTV"
  exit 0
fi
say "done - restarting ReplayOS in 3 s. Then open RetroTV in the system list."
restart_replay
