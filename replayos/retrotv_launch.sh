#!/usr/bin/env bash
# RetroTV for ReplayOS - start RetroTV remotely, optionally on a channel.
#
#   retrotv_launch.sh        start RetroTV (if it isn't running)
#   retrotv_launch.sh 36     ... and tune channel 36 (instant when it's already on)
#
# Needs RetroTV as its own system (ReplayOS 2.1.0+, retrotv_install.sh --slot system)
# or in the Virtual Boy slot (--slot vb or both): ReplayOS starts it through its own
# API (load_game), without autostart or a restart. The A/V test slot can't be started
# this way (ReplayOS refuses it). As a system it opens roms/retrotv/RetroTV.txt, else
# the first line-up there (RETROTV_GAME=<file in roms/retrotv> picks another).
#
# Installed to /media/sd/retrotv/ (the SD card), not the ReplayOS storage:
# when ReplayOS restarts it remounts NFS storage, and a script running from
# there would keep the share busy and push ReplayOS onto the SD card.
# Zaparoo cards: **execute:bash /media/sd/retrotv/retrotv_launch.sh 36
set -uo pipefail

CFG=/media/sd/config/replay.cfg
API=http://127.0.0.1:55356/api/v1
RTV=http://127.0.0.1:7474
LOG=/tmp/retrotv.log
cd /

log() { echo "$(date +%H:%M:%S) launcher: $*" >>"$LOG"; echo "[retrotv] $*"; }
token() { sed -n 's/^replay_http_token *= *"\{0,1\}\([^"]*\)"\{0,1\}.*/\1/p' "$CFG" | head -1; }
status() { curl -s -m 3 -H "X-RePlay-Token: $(token)" "$API/get_status"; }
rtv_up() { curl -s -m 2 "$RTV/status" >/dev/null 2>&1; }

CH=${1:-}
[[ $CH == "NULL" ]] && CH=""
[[ -z $CH || $CH =~ ^[A-Za-z0-9._-]+$ ]] || { log "not a channel number: $CH"; exit 1; }

if ! status | grep -q '"core_info":"RetroTV'; then
  if [[ -f /opt/replay/cores/retrotv_libretro.cfg ]]; then
    SYSTEM=retrotv GAME=${RETROTV_GAME:-}
    if [[ -z $GAME ]]; then                          # RetroTV.txt, else the first line-up
      kind=$(sed -n 's/^system_storage *= *"\{0,1\}\([a-z]*\)"\{0,1\}.*/\1/p' "$CFG" | head -1)
      dir=/media/${kind:-sd}/roms/retrotv
      GAME=RetroTV.txt
      [[ -f $dir/$GAME ]] || GAME=$(find "$dir" -maxdepth 1 -type f \( -iname '*.txt' -o -iname '*.m3u' -o -iname '*.m3u8' \) ! -name '.*' -printf '%f\n' 2>/dev/null | sort | head -1)
      [[ -n $GAME ]] || { log "no line-up in $dir - re-run retrotv_install.sh"; exit 1; }
    fi
  elif awk '/^\[/{s=($0=="[nintendo_vb]")} s && /retrotv_libretro\.so/' /opt/replay/cores/cores.cfg | grep -q .; then
    SYSTEM=nintendo_vb GAME=RetroTV.vb
  else
    log "RetroTV isn't installed as a system or in the Virtual Boy slot - run retrotv_install.sh (--slot system on ReplayOS 2.1.0+, else --slot vb)"
    exit 1
  fi
  [[ $GAME =~ ^[A-Za-z0-9._\ -]+$ ]] || { log "not a line-up file name: $GAME"; exit 1; }
  # after a restart (Zaparoo stops the active game that way) wait for the menu
  for _ in $(seq 1 45); do status | grep -q '"view"' && break; sleep 1; done
  log "starting RetroTV ($SYSTEM, $GAME)"
  ok=0
  for _ in $(seq 1 10); do
    r=$(curl -s -m 10 -w '%{http_code}' -o /tmp/retrotv-launch.out -H "X-RePlay-Token: $(token)" \
        -G --data-urlencode "system=$SYSTEM" --data-urlencode "game_file=$GAME" "$API/load_game")
    [[ $r == 200 ]] && { ok=1; break; }
    sleep 2
  done
  [[ $ok -eq 1 ]] || { log "ReplayOS refused load_game: $(cat /tmp/retrotv-launch.out 2>/dev/null)"; exit 1; }
fi

for _ in $(seq 1 40); do rtv_up && break; sleep 1; done    # remote control up = RetroTV loaded
rtv_up || { log "RetroTV started, but its remote control doesn't answer (System Settings > Remote control?)"; exit 0; }
if [[ -n $CH ]]; then
  log "tuning channel $CH"
  r=$(curl -s -m 3 "$RTV/tune?ch=$CH")
  if [[ $r != *'"ok":true'* ]]; then              # e.g. a card for a channel the line-up no longer has
    log "RetroTV didn't tune channel $CH: ${r:-no answer}"
    exit 1
  fi
fi
exit 0
