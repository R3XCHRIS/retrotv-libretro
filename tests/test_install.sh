#!/usr/bin/env bash
# Tests for replayos/retrotv_install.sh and retrotv_uninstall.sh: a fake ReplayOS
# (cores.cfg, storage, SD card) in a scratch folder, run as root in a user
# namespace (unshare -r), with dpkg-query, systemd-run and systemctl stubbed.
# Build: make test (skipped where user namespaces aren't allowed).
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
unshare -r true 2>/dev/null || { echo "test_install: skipped (no user namespaces here)"; exit 0; }

fails=0
check() { if eval "$1"; then :; else echo "FAIL ($case): $1" >&2; fails=$((fails + 1)); fi; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
CORES=$T/cores SDC=$T/sd ST=$T/storage BIN=$T/bin

fresh() {     # a clean ReplayOS: slots with their own cores, RetroTV files on the storage
  rm -rf "$CORES" "$SDC" "$ST" "$BIN" "$T/log"
  mkdir -p "$CORES" "$SDC" "$ST/roms/_extra" "$ST/retrotv" "$BIN"
  printf '[avtest]\nlow = "avtest_libretro.so"\nmid = "avtest_libretro.so"\nhi = "avtest_libretro.so"\n\n' >"$CORES/cores.cfg"
  printf '[nintendo_vb]\nlow = "mednafen_vb_libretro.so"\nmid = "mednafen_vb_libretro.so"\nhi = "mednafen_vb_libretro.so"\n\n' >>"$CORES/cores.cfg"
  printf '[pibench]\nlow = "pibench_libretro.so"\nmid = "pibench_libretro.so"\nhi = "pibench_libretro.so"\n' >>"$CORES/cores.cfg"
  cp replayos/retrotv_install.sh replayos/retrotv_uninstall.sh "$ST/roms/_extra/"
  cp replayos/retrotv_launch.sh replayos/retrotv-zaparoo.toml "$ST/retrotv/"
  echo core >"$ST/retrotv/retrotv_libretro.so"
  printf '#!/bin/sh\n[ -n "${FAKE_VERSION:-}" ] || exit 1\nprintf %%s "$FAKE_VERSION"\n' >"$BIN/dpkg-query"
  printf '#!/bin/sh\necho "systemd-run $*" >>"%s/log"\n' "$T" >"$BIN/systemd-run"
  printf '#!/bin/sh\necho "systemctl $*" >>"%s/log"\n' "$T" >"$BIN/systemctl"
  chmod +x "$BIN"/*
}
run() {       # version script args...
  local v=$1 s=$2
  shift 2
  PATH="$BIN:$PATH" FAKE_VERSION=$v RETROTV_CORES=$CORES RETROTV_SD=$SDC \
    unshare -r bash "$ST/roms/_extra/$s" "$@" >"$T/out" 2>&1
}
slot_core() { awk -v s="[$1]" '/^\[/{in_s=($0==s)} in_s && /^low/{gsub(/.*= *"|".*/, ""); print}' "$CORES/cores.cfg"; }

case="ReplayOS before 2.1.0 is refused"
fresh
run 2.0.60 retrotv_install.sh
check '[[ $? -ne 0 ]] && grep -q "needs ReplayOS 2.1.0 or newer (this is 2.0.60)" "$T/out"'
check '[[ ! -f $CORES/retrotv_libretro.cfg && ! -f $CORES/retrotv_libretro.so && ! -f $T/log ]]'
run "" retrotv_install.sh
check '[[ $? -ne 0 ]] && grep -q "this is unknown" "$T/out"'

case="fresh install: its own system"
fresh
run 2.1.0 retrotv_install.sh
check '[[ $? -eq 0 ]]'
check '[[ $(grep -v "^#" "$CORES/retrotv_libretro.cfg") == $(printf "id = \"retrotv\"\nname = \"RetroTV\"\nextensions = \"m3u|m3u8|txt\"\ndevices = \"pi5\"") ]]'
check '[[ $(stat -c %a "$CORES/retrotv_libretro.cfg") == 644 && ! -L $CORES/retrotv_libretro.cfg ]]'
check '! grep -q retrotv_libretro "$CORES/cores.cfg" && [[ $(slot_core avtest) == avtest_libretro.so ]]'
check '[[ -f $ST/roms/retrotv/RetroTV.txt && $(grep -vc "^#" "$ST/roms/retrotv/RetroTV.txt") -eq 0 ]]'
check '[[ -x $SDC/retrotv/retrotv_launch.sh && ! -f $ST/retrotv/slot.txt ]]'
check 'grep -q "RetroTV in the system list" "$T/out" && grep -q "systemd-run.*restart replay.service" "$T/log"'
run 2.1.0 retrotv_install.sh
check '[[ $? -eq 0 && $(ls "$ST/roms/retrotv" | wc -l) -eq 1 ]]'      # re-run: nothing doubled
fresh
run 2.1.3 retrotv_install.sh
check '[[ -f $CORES/retrotv_libretro.cfg ]]'                              # later releases too

case="an old slot install is moved over"
fresh
sed -i '/^\[nintendo_vb\]/,/^$/s/mednafen_vb_libretro\.so/retrotv_libretro.so/; /^\[avtest\]/,/^$/s/avtest_libretro\.so/retrotv_libretro.so/' "$CORES/cores.cfg"
mkdir -p "$ST/roms/nintendo_vb" "$ST/config/settings/system/crt" "$ST/saves/nintendo_vb" "$SDC/zaparoo/launchers"
: >"$ST/roms/nintendo_vb/RetroTV.vb"
echo both >"$ST/retrotv/slot.txt"
echo 'replay_video_monitor_multi_mode = "2"' >"$ST/config/settings/system/crt/nintendo_vb.cfg"
echo 5 >"$ST/saves/nintendo_vb/retrotv-last-1234abcd.txt"
run 2.1.0 retrotv_install.sh --slot both
check '[[ $? -eq 0 && -f $CORES/retrotv_libretro.cfg ]] && grep -q "no longer needed" "$T/out"'
check '[[ $(slot_core nintendo_vb) == mednafen_vb_libretro.so && $(slot_core avtest) == avtest_libretro.so ]]'
check '! grep -q retrotv_libretro "$CORES/cores.cfg" && ls "$ST"/retrotv/backup/cores.cfg.* >/dev/null'
check '[[ ! -f $ST/roms/nintendo_vb/RetroTV.vb && ! -f $ST/retrotv/slot.txt ]]'
check 'grep -q multi_mode "$ST/config/settings/system/crt/retrotv.cfg"'
check '[[ $(cat "$ST/saves/retrotv/retrotv-last-1234abcd.txt") == 5 ]]'
check '[[ -x $SDC/retrotv/retrotv_launch.sh && -f $SDC/zaparoo/launchers/retrotv.toml ]]'

case="--no-restart (RetroTV's own updater)"
fresh
run 2.1.0 retrotv_install.sh --no-restart
check '[[ $? -eq 3 && ! -f $T/log ]] && grep -q "restart ReplayOS to finish" "$T/out"'     # new system: restart needed
run 2.1.0 retrotv_install.sh --no-restart
check '[[ $? -eq 0 && ! -f $T/log ]] && grep -q "next time you open RetroTV" "$T/out"'     # an update: none
check '[[ -f $CORES/retrotv_libretro.cfg ]]'

case="your own line-ups are left alone"
fresh
mkdir -p "$ST/roms/retrotv"
echo "http://tv.example/home.m3u" >"$ST/roms/retrotv/Home TV.txt"
run 2.1.0 retrotv_install.sh
check '[[ ! -f $ST/roms/retrotv/RetroTV.txt && -f "$ST/roms/retrotv/Home TV.txt" ]]'

case="uninstall"
fresh
run 2.1.0 retrotv_install.sh
: >"$T/log"
run 2.1.0 retrotv_uninstall.sh
check '[[ $? -eq 0 && ! -f $CORES/retrotv_libretro.cfg && ! -f $CORES/retrotv_libretro.so ]]'
check '[[ ! -f $SDC/retrotv/retrotv_launch.sh && -f $ST/roms/retrotv/RetroTV.txt ]]'   # line-ups kept
check '! grep -q retrotv_libretro "$CORES/cores.cfg" && grep -q "systemd-run.*restart replay.service" "$T/log"'

if [[ $fails -gt 0 ]]; then echo "$fails failure(s)" >&2; exit 1; fi
echo "test_install: all passed"
