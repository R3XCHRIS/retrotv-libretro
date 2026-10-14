#!/usr/bin/env bash
# Run the core headlessly on the Pi (outside ReplayOS) and fetch screenshots.
#
#   PI=root@192.168.100.169 PLAYLIST=tests/local/channels.m3u scripts/pi-test.sh
#
# SCRIPT button names are libretro ids (harness): pad A = "b", pad B = "a",
# pad Y = "x", pad X = "y".
#
# OPTS="retrotv_theme=teletext,..." sets core options for the run.
#
# Writes screenshots, the run log (run.log) and per-thread CPU samples
# (cpu.txt, read by scripts/cpu-summary.py) to $OUT.
#
# Nothing is installed on the Pi: files go to /tmp/retrotv-test and are removed
# afterwards. ReplayOS keeps running; the harness only uses the network and CPU.
set -euo pipefail
cd "$(dirname "$0")/.."
PI="${PI:-root@192.168.100.169}"
PLAYLIST="${PLAYLIST:-tests/local/channels.m3u}"
SECONDS_TO_RUN="${SECONDS_TO_RUN:-60}"
SCRIPT="${SCRIPT:-shot@8 up@10 shot@20 up@24 shot@34 up@36 shot@44 a@46 shot@52 select@54 shot@55 a@56 b@57 shot@58}"
REMOTE=/tmp/retrotv-test
OUT="${OUT:-build/pi-shots}"

[[ -f build/retrotv_libretro.so && -f build/harness ]] || { echo "run 'make' first"; exit 1; }
[[ -f "$PLAYLIST" ]] || { echo "playlist not found: $PLAYLIST (see tests/local/README)"; exit 1; }

ssh -o BatchMode=yes "$PI" "rm -rf $REMOTE && mkdir -p $REMOTE/shots"
scp -q -o BatchMode=yes build/retrotv_libretro.so build/harness scripts/pi-run.sh "$PI:$REMOTE/"
scp -q -o BatchMode=yes "$PLAYLIST" "$PI:$REMOTE/channels.m3u"
ssh -o BatchMode=yes "$PI" "HARNESS_OPTS='${OPTS:-}' sh $REMOTE/pi-run.sh $SECONDS_TO_RUN '$SCRIPT'"
rm -rf "$OUT" && mkdir -p "$OUT"
scp -q -o BatchMode=yes "$PI:$REMOTE/shots/*.png" "$PI:$REMOTE/run.log" "$PI:$REMOTE/cpu.txt" "$OUT/" 2>/dev/null || true
grep -E "playing ch|NO SIGNAL|stats:" "$OUT/run.log" || true
python3 scripts/cpu-summary.py "$OUT/cpu.txt" || true
ssh -o BatchMode=yes "$PI" "rm -rf $REMOTE"
echo "screenshots: $(ls "$OUT" | wc -l) in $OUT"
