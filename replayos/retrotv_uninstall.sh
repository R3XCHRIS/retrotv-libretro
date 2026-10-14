#!/usr/bin/env bash
# RetroTV for ReplayOS - give borrowed slots back: [avtest] to the A/V test
# pattern and [pibench] (older installs) to PiBench.
# Leaves <storage>/retrotv (core copy, playlist, backups) in place.
set -euo pipefail
[[ "${1:-}" == "NULL" ]] && shift

CORES=/opt/replay/cores
CFG=$CORES/cores.cfg
say() { echo "[retrotv] $*"; }
die() { echo "[retrotv] ERROR: $*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || die "run as root"
[[ -f $CFG ]] || die "$CFG not found"

cp -p "$CFG" "$CFG.retrotv-uninstall.$(date +%Y%m%d-%H%M%S)"
tmp=$(mktemp)
# each borrowed slot goes back to its own core: [avtest] -> avtest_libretro.so, [pibench] -> pibench_libretro.so
awk '
  /^\[/ { sec = substr($0, 2, length($0) - 2) }
  (sec == "avtest" || sec == "pibench") && /retrotv_libretro\.so/ { split($0, kv, "="); gsub(/[[:space:]]/, "", kv[1]); print kv[1] " = \"" sec "_libretro.so\""; next }
  { print }' "$CFG" >"$tmp"
cat "$tmp" >"$CFG"
rm -f "$tmp"
grep -q retrotv_libretro "$CFG" && die "cores.cfg still mentions RetroTV - check $CFG"
rm -f "$CORES/retrotv_libretro.so"
say "[avtest] and [pibench] restored, core removed"
say "restarting ReplayOS in 3 s"
( sleep 3; systemctl restart replay.service ) >/dev/null 2>&1 &
