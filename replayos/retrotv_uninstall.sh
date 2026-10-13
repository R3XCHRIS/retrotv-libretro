#!/usr/bin/env bash
# RetroTV for ReplayOS - give the "pibench" slot back to PiBench.
# Leaves <storage>/retrotv (core copy, playlist, backups) in place.
set -euo pipefail
[[ "${1:-}" == "NULL" ]] && shift

CORES=/opt/replay/cores
CFG=$CORES/cores.cfg
SLOT=pibench
say() { echo "[retrotv] $*"; }
die() { echo "[retrotv] ERROR: $*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || die "run as root"
[[ -f $CFG ]] || die "$CFG not found"

cp -p "$CFG" "$CFG.retrotv-uninstall.$(date +%Y%m%d-%H%M%S)"
tmp=$(mktemp)
awk -v slot="[$SLOT]" '
  /^\[/ { insec = ($0 == slot) }
  insec && /retrotv_libretro\.so/ { split($0, kv, "="); gsub(/[[:space:]]/, "", kv[1]); print kv[1] " = \"pibench_libretro.so\""; next }
  { print }' "$CFG" >"$tmp"
cat "$tmp" >"$CFG"
rm -f "$tmp"
rm -f "$CORES/retrotv_libretro.so"
grep -q retrotv_libretro "$CFG" && die "cores.cfg still mentions RetroTV - check $CFG"
say "[$SLOT] restored to PiBench, core removed"
say "restarting ReplayOS in 3 s"
( sleep 3; systemctl restart replay.service ) >/dev/null 2>&1 &
