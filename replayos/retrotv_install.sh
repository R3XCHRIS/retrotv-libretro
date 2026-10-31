#!/usr/bin/env bash
# RetroTV for ReplayOS - install as its own system, or into a borrowed system slot.
#
#   retrotv_install.sh [--slot system|avtest|vb|both] [--no-restart]
#
#   --no-restart  don't restart ReplayOS (RetroTV's own updater): the new core is used
#           when RetroTV is next opened. Exits 3 if a restart is still needed because
#           the system list changes (cores.cfg or the manifest).
#
#   system  RetroTV as its own system (ReplayOS 2.1.0 or newer): "RetroTV" in the system
#           list, with your line-ups as its games (roms/retrotv/*.txt, *.m3u). Remote
#           start (load_game), the launcher and Zaparoo work. The default on 2.1.0+.
#   avtest  Extra > audio_video_test (the A/V test pattern's slot). Start it from the menu.
#   vb      Nintendo Virtual Boy: "RetroTV" is the one game there. ReplayOS can start it
#           remotely (its load_game API), so the RetroTV launcher and Zaparoo cards work.
#           Refused if you have Virtual Boy games.
#   both    both of the above.
# Without --slot, the last choice (<storage>/retrotv/slot.txt) is kept; the first time,
# system on ReplayOS 2.1.0+, else avtest.
#
# Put this file in <storage>/roms/_extra/ and run it from the ReplayOS
# Extra menu (or over SSH as root). With a borrowed slot, re-run it after every
# ReplayOS update: updates restore cores.cfg. The system install survives updates.
#
# Needs <storage>/retrotv/retrotv_libretro.so (storage = the ReplayOS storage
# root: /media/nfs, /media/usb, /media/nvme or /media/sd).
#
# What it changes (with backups):
#   /opt/replay/cores/retrotv_libretro.so        added
#   /opt/replay/cores/retrotv_libretro.cfg       the system's manifest (system; removed otherwise)
#   <storage>/roms/retrotv/RetroTV.txt           a line-up using retrotv/playlist.txt (system,
#                                                when the folder has no playlists yet)
#   /opt/replay/cores/cores.cfg [avtest]         low/mid/hi -> retrotv_libretro.so (avtest, both)
#   /opt/replay/cores/cores.cfg [nintendo_vb]    low/mid/hi -> retrotv_libretro.so (vb, both)
#   <storage>/roms/nintendo_vb/RetroTV.vb        the "game" (empty; vb, both)
#   /opt/replay/cores/cores.cfg [pibench]        back to pibench_libretro.so if an
#                                                older RetroTV install had taken it
# A slot no longer chosen gets its own core back: an external system can't share its
# core with cores.cfg. RetroTV's settings and saves are per system: a new one starts
# with a copy of the previous one's. system and vb also install the remote launcher
# (/media/sd/retrotv/retrotv_launch.sh) and, with Zaparoo, a RetroTV system there.
# Why the slots: before 2.1.0 ReplayOS can't add systems, mutes the pibench slot, and
# its API refuses the Extras systems, so remote start needs a regular system's slot.
set -euo pipefail
[[ "${1:-}" == "NULL" ]] && shift   # ReplayOS passes NULL when launched from the menu

CORES=${RETROTV_CORES:-/opt/replay/cores}            # tests point these at a scratch tree
SD=${RETROTV_SD:-/media/sd}
CFG=$CORES/cores.cfg
MANIFEST=$CORES/retrotv_libretro.cfg
NOW=$(date +%Y%m%d-%H%M%S)
SLOT=""
NORESTART=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --slot) [[ $# -ge 2 ]] || { echo "[retrotv] ERROR: --slot needs system, avtest, vb or both" >&2; exit 1; }; SLOT=$2; shift ;;
    --no-restart) NORESTART=1 ;;
    NULL) ;;
    *) echo "[retrotv] ERROR: unknown option: $1" >&2; exit 1 ;;
  esac
  shift
done

say() { echo "[retrotv] $*"; }
die() { echo "[retrotv] ERROR: $*" >&2; exit 1; }
replay_version() { dpkg-query -W -f='${Version}' replay-frontend 2>/dev/null || true; }
# external systems (a _libretro.cfg manifest next to the core) arrived in ReplayOS 2.1.0
has_systems() {
  local v
  v=$(replay_version)
  [[ -n $v && $(printf '%s\n' 2.1.0 "$v" | sort -V | head -1) == 2.1.0 ]]
}
# restart ReplayOS from a systemd timer: nothing of ours is left running on the
# storage when ReplayOS remounts it (a busy NFS share pushes it onto the SD card)
restart_replay() {
  cd /
  systemd-run --quiet --on-active=3 --unit="retrotv-restart-$(date +%s)" systemctl restart replay.service 2>/dev/null ||
    ( sleep 3; systemctl restart replay.service ) </dev/null >/dev/null 2>&1 &
}

[[ $EUID -eq 0 ]] || die "run as root"
[[ -f $CFG ]] || die "$CFG not found - is this ReplayOS?"

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

[[ -z $SLOT && -f $STORAGE/retrotv/slot.txt ]] && SLOT=$(tr -d '[:space:]' <"$STORAGE/retrotv/slot.txt")
if [[ -z $SLOT ]]; then has_systems && SLOT=system || SLOT=avtest; fi
case $SLOT in system|avtest|vb|both) ;; *) die "--slot must be system, avtest, vb or both (not '$SLOT')" ;; esac
if [[ $SLOT == system ]] && ! has_systems; then
  v=$(replay_version)
  die "--slot system needs ReplayOS 2.1.0 or newer (this is ${v:-unknown}); use --slot avtest or vb"
fi
WANT_SYS=0; WANT_AV=0; WANT_VB=0
[[ $SLOT == system ]] && WANT_SYS=1
[[ $SLOT == avtest || $SLOT == both ]] && WANT_AV=1
[[ $SLOT == vb || $SLOT == both ]] && WANT_VB=1
REMOTE=$((WANT_SYS | WANT_VB))                       # load_game works for these
for sec in avtest nintendo_vb; do
  grep -q "^\[$sec\]" "$CFG" || die "no [$sec] section in $CFG - ReplayOS changed; not installing"
done
VBDIR=$STORAGE/roms/nintendo_vb
if [[ $WANT_VB -eq 1 && -d $VBDIR ]]; then
  others=$(find "$VBDIR" -type f ! -name 'RetroTV.vb' ! -name '.*' | head -3)
  [[ -z $others ]] || die "you have Virtual Boy games in $VBDIR - not taking that slot (use --slot avtest)"
fi
say "slot: $SLOT"

mkdir -p "$STORAGE/retrotv/backup"
# what ReplayOS reads at start-up: if neither changes, no restart is needed
startup_files() { { cat "$CFG"; cat "$MANIFEST" 2>/dev/null || true; } | md5sum; }
before=$(startup_files)
# plain cp: NFS shares usually squash root, so ownership can't be preserved there
[[ -f "$STORAGE/retrotv/backup/cores.cfg.original" ]] || cp "$CFG" "$STORAGE/retrotv/backup/cores.cfg.original"
cp "$CFG" "$STORAGE/retrotv/backup/cores.cfg.$NOW"
[[ -f "$CFG.pre-retrotv" ]] || cp -p "$CFG" "$CFG.pre-retrotv"
say "backed up cores.cfg"

install -m 0644 "$SRC" "$CORES/retrotv_libretro.so.new"
mv -f "$CORES/retrotv_libretro.so.new" "$CORES/retrotv_libretro.so"
say "installed $CORES/retrotv_libretro.so"

# RetroTV as its own system: the manifest ReplayOS reads at start-up, next to the core
SYSDIR=$STORAGE/roms/retrotv
if [[ $WANT_SYS -eq 1 ]]; then
  printf '%s\n' '# RetroTV as its own system (ReplayOS 2.1.0+). Written by retrotv_install.sh.' \
    'id = "retrotv"' 'name = "RetroTV"' 'extensions = "m3u|m3u8|txt"' 'devices = "pi5"' >"$MANIFEST.new"
  chmod 0644 "$MANIFEST.new"
  mv -f "$MANIFEST.new" "$MANIFEST"
  mkdir -p "$SYSDIR"
  if ! find "$SYSDIR" -maxdepth 1 -type f \( -iname '*.txt' -o -iname '*.m3u' -o -iname '*.m3u8' \) ! -name '.*' | grep -q .; then
    printf '%s\n' '# RetroTV: open this to watch the channels in <storage>/retrotv/playlist.txt.' \
      '# To make it a line-up of its own, put an M3U link (or a path) on a line below,' \
      '# and optionally epg=<guide link>. Each .txt or .m3u file here is a line-up.' >"$SYSDIR/RetroTV.txt"
    say "added $SYSDIR/RetroTV.txt (uses retrotv/playlist.txt)"
  fi
  say "registered RetroTV as a system ($MANIFEST)"
elif [[ -f $MANIFEST ]]; then
  rm -f "$MANIFEST"
  say "RetroTV is no longer its own system (manifest removed)"
fi

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

has_retrotv() { awk -v slot="[$1]" '/^\[/{insec=($0==slot)} insec && /retrotv_libretro\.so/' "$CFG" | wc -l; }
# a slot's own core, from cores.cfg as it was before RetroTV (else the known default)
own_core() {
  local c=""
  [[ -f $STORAGE/retrotv/backup/cores.cfg.original ]] &&
    c=$(awk -v slot="[$1]" '/^\[/{insec=($0==slot)} insec && /^low[[:space:]]*=/{gsub(/.*= *"|".*/, ""); print; exit}' "$STORAGE/retrotv/backup/cores.cfg.original")
  [[ -n $c && $c != retrotv_libretro.so ]] && echo "$c" || echo "$2"
}

if [[ $(has_retrotv pibench) -gt 0 ]]; then
  set_slot pibench pibench_libretro.so
  say "[pibench] given back to PiBench (older RetroTV install)"
fi
if [[ $WANT_AV -eq 1 ]]; then
  set_slot avtest retrotv_libretro.so
  [[ $(has_retrotv avtest) -eq 3 ]] || { cat "$STORAGE/retrotv/backup/cores.cfg.$NOW" >"$CFG"; die "cores.cfg edit failed - restored backup"; }
  say "[avtest] now launches RetroTV (Extra > audio_video_test)"
elif [[ $(has_retrotv avtest) -gt 0 ]]; then
  set_slot avtest avtest_libretro.so
  say "[avtest] given back to the A/V test pattern"
fi
if [[ $WANT_VB -eq 1 ]]; then
  set_slot nintendo_vb retrotv_libretro.so
  [[ $(has_retrotv nintendo_vb) -eq 3 ]] || { cat "$STORAGE/retrotv/backup/cores.cfg.$NOW" >"$CFG"; die "cores.cfg edit failed - restored backup"; }
  mkdir -p "$VBDIR"
  [[ -f $VBDIR/RetroTV.vb ]] || : >"$VBDIR/RetroTV.vb"
  say "[nintendo_vb] now launches RetroTV (Nintendo Virtual Boy > RetroTV)"
elif [[ $(has_retrotv nintendo_vb) -gt 0 ]]; then
  set_slot nintendo_vb "$(own_core nintendo_vb mednafen_vb_libretro.so)"
  rm -f "$VBDIR/RetroTV.vb"
  say "[nintendo_vb] given back to Virtual Boy"
fi
echo "$SLOT" >"$STORAGE/retrotv/slot.txt"

# RetroTV's settings and saves are per system: a slot used for the first time starts
# with a copy of the other slot's (System Settings, last channel, screen 2, zoom)
seed_slot() {   # from to
  local d
  for d in "$STORAGE"/config/settings/system/*; do
    [[ -f $d/$1.cfg && ! -f $d/$2.cfg ]] && cp "$d/$1.cfg" "$d/$2.cfg" && say "settings copied: $(basename "$d")/$1.cfg -> $2.cfg"
  done
  if ls "$STORAGE/saves/$1"/retrotv-* >/dev/null 2>&1 && ! ls "$STORAGE/saves/$2"/retrotv-* >/dev/null 2>&1; then
    mkdir -p "$STORAGE/saves/$2" && cp "$STORAGE/saves/$1"/retrotv-* "$STORAGE/saves/$2/" && say "saves copied: $1 -> $2"
  fi
  return 0                                         # nothing to copy is fine (set -e)
}
# (nintendo_vb first: the slot the two-screen setup used)
[[ $WANT_SYS -eq 1 ]] && { seed_slot nintendo_vb retrotv; seed_slot avtest retrotv; }
[[ $WANT_VB -eq 1 ]] && { seed_slot retrotv nintendo_vb; seed_slot avtest nintendo_vb; }
[[ $WANT_AV -eq 1 ]] && { seed_slot retrotv avtest; seed_slot nintendo_vb avtest; }

# Remote start (system or Virtual Boy slot): the launcher lives on the SD card, because
# a ReplayOS restart remounts NFS storage and a script running from there blocks it
LAUNCH=$SD/retrotv/retrotv_launch.sh
if [[ $REMOTE -eq 1 && -f $STORAGE/retrotv/retrotv_launch.sh ]]; then
  mkdir -p "$SD/retrotv"
  cp "$STORAGE/retrotv/retrotv_launch.sh" "$LAUNCH" && chmod 755 "$LAUNCH"
elif [[ $REMOTE -eq 0 ]]; then
  rm -f "$LAUNCH"
fi

# Zaparoo (optional): "RetroTV" as a system, and **execute cards for channels
ZDIR=$SD/zaparoo
ZRULE="allow_execute = ['^bash /media/sd/retrotv/retrotv_launch\\.sh( [A-Za-z0-9._-]+)?\$']"
if [[ -d $ZDIR/launchers ]]; then
  if [[ $REMOTE -eq 1 && -f $STORAGE/retrotv/retrotv-zaparoo.toml ]]; then
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
  elif [[ $REMOTE -eq 0 ]] && { [[ -f $ZDIR/launchers/retrotv.toml ]] || grep -qs 'retrotv_launch' "$ZDIR/config.toml"; }; then
    rm -f "$ZDIR/launchers/retrotv.toml"
    if grep -qs 'retrotv_launch' "$ZDIR/config.toml"; then          # our allow_execute line
      grep -v 'retrotv_launch' "$ZDIR/config.toml" >"$ZDIR/config.toml.new" && cat "$ZDIR/config.toml.new" >"$ZDIR/config.toml"
      rm -f "$ZDIR/config.toml.new"
    fi
    systemctl try-restart zaparoo.service >/dev/null 2>&1 || true
    say "Zaparoo launcher removed (it needs the RetroTV system or the Virtual Boy slot)"
  fi
fi

if [[ $WANT_SYS -eq 1 ]]; then where="RetroTV in the system list"
elif [[ $WANT_AV -eq 1 ]]; then where="Extra > audio_video_test"
else where="Nintendo Virtual Boy > RetroTV"; fi
[[ $REMOTE -eq 1 ]] && say "remote start: bash $LAUNCH [channel]"
if [[ $NORESTART -eq 1 ]]; then
  if [[ $(startup_files) != "$before" ]]; then
    say "done - restart ReplayOS to finish (the system list changed). Then open $where."
    exit 3
  fi
  say "done - the new version starts the next time you open RetroTV"
  exit 0
fi
say "done - restarting ReplayOS in 3 s. Then open $where."
restart_replay
