#!/usr/bin/env bash
# RetroTV for ReplayOS - remove the RetroTV system: the core and its manifest, the
# remote launcher and the Zaparoo launcher. Restarts ReplayOS.
# Leaves <storage>/retrotv (core copy, playlist, backups), roms/retrotv (your line-ups)
# and RetroTV's settings and saves in place. A slot an old RetroTV version borrowed
# (A/V test, Virtual Boy, PiBench) gets its own core back too, should one be left.
set -euo pipefail
[[ "${1:-}" == "NULL" ]] && shift

CORES=${RETROTV_CORES:-/opt/replay/cores}            # tests point these at a scratch tree
SD=${RETROTV_SD:-/media/sd}
CFG=$CORES/cores.cfg
say() { echo "[retrotv] $*"; }
die() { echo "[retrotv] ERROR: $*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || die "run as root"
[[ -f $CFG ]] || die "$CFG not found"

cp -p "$CFG" "$CFG.retrotv-uninstall.$(date +%Y%m%d-%H%M%S)"
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
STORAGE=$(cd "$HERE/../.." && pwd)                 # this script lives in <storage>/roms/_extra
# Virtual Boy's own core, from cores.cfg as it was before RetroTV (else the known default)
VBCORE=mednafen_vb_libretro.so
if [[ -f $STORAGE/retrotv/backup/cores.cfg.original ]]; then
  c=$(awk '/^\[/{s=($0=="[nintendo_vb]")} s && /^low[[:space:]]*=/{gsub(/.*= *"|".*/, ""); print; exit}' "$STORAGE/retrotv/backup/cores.cfg.original")
  [[ -n $c && $c != retrotv_libretro.so ]] && VBCORE=$c
fi
tmp=$(mktemp)
# a slot an old version borrowed goes back to its own core: [avtest] -> avtest_libretro.so, [pibench] -> pibench_libretro.so
awk -v vbcore="$VBCORE" '
  /^\[/ { sec = substr($0, 2, length($0) - 2) }
  (sec == "avtest" || sec == "pibench") && /retrotv_libretro\.so/ { split($0, kv, "="); gsub(/[[:space:]]/, "", kv[1]); print kv[1] " = \"" sec "_libretro.so\""; next }
  sec == "nintendo_vb" && /retrotv_libretro\.so/ { split($0, kv, "="); gsub(/[[:space:]]/, "", kv[1]); print kv[1] " = \"" vbcore "\""; next }
  { print }' "$CFG" >"$tmp"
cat "$tmp" >"$CFG"
rm -f "$tmp"
grep -q retrotv_libretro "$CFG" && die "cores.cfg still mentions RetroTV - check $CFG"
rm -f "$CORES/retrotv_libretro.cfg" "$CORES/retrotv_libretro.so" "$STORAGE/roms/nintendo_vb/RetroTV.vb" "$STORAGE/retrotv/slot.txt"
rm -f "$SD/retrotv/retrotv_launch.sh"
if [[ -f $SD/zaparoo/launchers/retrotv.toml ]] || grep -qs 'retrotv_launch' "$SD/zaparoo/config.toml"; then
  rm -f "$SD/zaparoo/launchers/retrotv.toml"
  Z=$SD/zaparoo/config.toml                                         # our allow_execute line
  if grep -qs 'retrotv_launch' "$Z"; then grep -v 'retrotv_launch' "$Z" >"$Z.new" && cat "$Z.new" >"$Z"; rm -f "$Z.new"; fi
  systemctl try-restart zaparoo.service >/dev/null 2>&1 || true
  say "Zaparoo launcher removed"
fi
say "RetroTV removed (the core, its system and the launchers)"
say "restarting ReplayOS in 3 s"
# from a systemd timer: nothing of ours is left running on the storage when ReplayOS remounts it
cd /
systemd-run --quiet --on-active=3 --unit="retrotv-restart-$(date +%s)" systemctl restart replay.service 2>/dev/null ||
  ( sleep 3; systemctl restart replay.service ) </dev/null >/dev/null 2>&1 &
