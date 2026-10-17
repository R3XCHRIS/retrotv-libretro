#!/usr/bin/env bash
# RetroTV for ReplayOS - start RetroTV remotely, optionally on a channel.
#
#   retrotv_launch.sh        start RetroTV (if it isn't running)
#   retrotv_launch.sh 36     ... and tune channel 36 (instant when it's already on)
#
# Needs RetroTV in the Virtual Boy slot (retrotv_install.sh --slot vb or both):
# ReplayOS starts it through its own API (load_game), without autostart or a
# restart. The A/V test slot can't be started this way (ReplayOS refuses it).
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
  if ! awk '/^\[/{s=($0=="[nintendo_vb]")} s && /retrotv_libretro\.so/' /opt/replay/cores/cores.cfg | grep -q .; then
    log "RetroTV isn't in the Virtual Boy slot - run retrotv_install.sh --slot vb (or both)"
    exit 1
  fi
  # after a restart (Zaparoo stops the active game that way) wait for the menu
  for _ in $(seq 1 45); do status | grep -q '"view"' && break; sleep 1; done
  log "starting RetroTV (Virtual Boy slot)"
  ok=0
  for _ in $(seq 1 10); do
    r=$(curl -s -m 10 -w '%{http_code}' -o /tmp/retrotv-launch.out -H "X-RePlay-Token: $(token)" \
        "$API/load_game?system=nintendo_vb&game_file=RetroTV.vb")
    [[ $r == 200 ]] && { ok=1; break; }
    sleep 2
  done
  [[ $ok -eq 1 ]] || { log "ReplayOS refused load_game: $(cat /tmp/retrotv-launch.out 2>/dev/null)"; exit 1; }
fi

for _ in $(seq 1 40); do rtv_up && break; sleep 1; done    # remote control up = RetroTV loaded
rtv_up || { log "RetroTV started, but its remote control doesn't answer (System Settings > Remote control?)"; exit 0; }
if [[ -n $CH ]]; then
  log "tuning channel $CH"
  curl -s -m 3 "$RTV/tune?ch=$CH" >/dev/null
fi
exit 0
