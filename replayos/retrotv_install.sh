#!/usr/bin/env bash
# RetroTV for ReplayOS - install into a borrowed system slot.
#
#   retrotv_install.sh [--slot avtest|vb|both]
#
#   avtest  Extra > audio_video_test (the A/V test pattern's slot). Start it from the menu.
#   vb      Nintendo Virtual Boy: "RetroTV" is the one game there. ReplayOS can start it
#           remotely (its load_game API), so the RetroTV launcher and Zaparoo cards work.
#           Refused if you have Virtual Boy games.
#   both    both of the above.
# Without --slot, the last choice (<storage>/retrotv/slot.txt) is kept; the first time, avtest.
#
# Put this file in <storage>/roms/_extra/ and run it from the ReplayOS
# Extra menu (or over SSH as root). Re-run it after every ReplayOS update:
# updates replace /opt/replay, which undoes the install.
#
# Needs <storage>/retrotv/retrotv_libretro.so (storage = the ReplayOS storage
# root: /media/nfs, /media/usb, /media/nvme or /media/sd).
#
# What it changes (with backups):
#   /opt/replay/cores/retrotv_libretro.so        added
#   /opt/replay/cores/cores.cfg [avtest]         low/mid/hi -> retrotv_libretro.so (avtest, both)
#   /opt/replay/cores/cores.cfg [nintendo_vb]    low/mid/hi -> retrotv_libretro.so (vb, both)
#   <storage>/roms/nintendo_vb/RetroTV.vb        the "game" (empty; vb, both)
#   /opt/replay/cores/cores.cfg [pibench]        back to pibench_libretro.so if an
#                                                older RetroTV install had taken it
# A slot no longer chosen gets its own core back. RetroTV's settings and saves are
# per slot: a new slot starts with a copy of the other's. vb also installs the remote
# launcher (/media/sd/retrotv/retrotv_launch.sh) and, with Zaparoo, a RetroTV system there.
# Why these slots: ReplayOS can't add systems, mutes the pibench slot, and its API
# refuses the Extras systems, so remote start needs a regular system's slot.
set -euo pipefail
[[ "${1:-}" == "NULL" ]] && shift   # ReplayOS passes NULL when launched from the menu

CORES=/opt/replay/cores
CFG=$CORES/cores.cfg
NOW=$(date +%Y%m%d-%H%M%S)
SLOT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --slot) [[ $# -ge 2 ]] || { echo "[retrotv] ERROR: --slot needs avtest, vb or both" >&2; exit 1; }; SLOT=$2; shift ;;
    NULL) ;;
    *) echo "[retrotv] ERROR: unknown option: $1" >&2; exit 1 ;;
  esac
  shift
done

say() { echo "[retrotv] $*"; }
die() { echo "[retrotv] ERROR: $*" >&2; exit 1; }

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
SLOT=${SLOT:-avtest}
case $SLOT in avtest|vb|both) ;; *) die "--slot must be avtest, vb or both (not '$SLOT')" ;; esac
WANT_AV=0; WANT_VB=0
[[ $SLOT == avtest || $SLOT == both ]] && WANT_AV=1
[[ $SLOT == vb || $SLOT == both ]] && WANT_VB=1
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
[[ $WANT_VB -eq 1 ]] && seed_slot avtest nintendo_vb
[[ $WANT_AV -eq 1 ]] && seed_slot nintendo_vb avtest

# Remote start (Virtual Boy slot): the launcher lives on the SD card, because a
# ReplayOS restart remounts NFS storage and a script running from there blocks it
LAUNCH=/media/sd/retrotv/retrotv_launch.sh
if [[ $WANT_VB -eq 1 && -f $STORAGE/retrotv/retrotv_launch.sh ]]; then
  mkdir -p /media/sd/retrotv
  cp "$STORAGE/retrotv/retrotv_launch.sh" "$LAUNCH" && chmod 755 "$LAUNCH"
elif [[ $WANT_VB -eq 0 ]]; then
  rm -f "$LAUNCH"
fi

# Zaparoo (optional): "RetroTV" as a system, and **execute cards for channels
ZDIR=/media/sd/zaparoo
ZRULE="allow_execute = ['^bash /media/sd/retrotv/retrotv_launch\\.sh( [A-Za-z0-9._-]+)?\$']"
if [[ -d $ZDIR/launchers ]]; then
  if [[ $WANT_VB -eq 1 && -f $STORAGE/retrotv/retrotv-zaparoo.toml ]]; then
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
  elif [[ $WANT_VB -eq 0 ]] && { [[ -f $ZDIR/launchers/retrotv.toml ]] || grep -qs 'retrotv_launch' "$ZDIR/config.toml"; }; then
    rm -f "$ZDIR/launchers/retrotv.toml"
    if grep -qs 'retrotv_launch' "$ZDIR/config.toml"; then          # our allow_execute line
      grep -v 'retrotv_launch' "$ZDIR/config.toml" >"$ZDIR/config.toml.new" && cat "$ZDIR/config.toml.new" >"$ZDIR/config.toml"
      rm -f "$ZDIR/config.toml.new"
    fi
    systemctl try-restart zaparoo.service >/dev/null 2>&1 || true
    say "Zaparoo launcher removed (it needs the Virtual Boy slot)"
  fi
fi

if [[ $WANT_AV -eq 1 ]]; then where="Extra > audio_video_test"; else where="Nintendo Virtual Boy > RetroTV"; fi
[[ $WANT_VB -eq 1 ]] && say "remote start: bash $LAUNCH [channel]"
say "done - restarting ReplayOS in 3 s. Then open $where."
( sleep 3; systemctl restart replay.service ) >/dev/null 2>&1 &
