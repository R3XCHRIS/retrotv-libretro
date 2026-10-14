#!/usr/bin/env bash
# Take the documentation screenshots on the Pi with the demo line-up
# (tests/demo/make-demo.sh): no private channels, so the PNGs can be
# committed. Runs the headless harness in /tmp on the Pi; ReplayOS keeps
# running. Output: docs/screenshots/.
#
#   PI=root@<pi-address> scripts/demo-shots.sh
set -euo pipefail
cd "$(dirname "$0")/.."
PI="${PI:-root@replay.local}"
REMOTE=/tmp/rtdemo
OUT=docs/screenshots
[[ -f build/retrotv_libretro.so && -f build/harness ]] || { echo "run 'make' first"; exit 1; }
[[ -f build/demo/demo.m3u ]] || tests/demo/make-demo.sh
ssh -o BatchMode=yes "$PI" "rm -rf $REMOTE && mkdir -p $REMOTE"
scp -q -o BatchMode=yes build/retrotv_libretro.so build/harness build/demo/* "$PI:$REMOTE/"
ssh -o BatchMode=yes "$PI" "cd $REMOTE && chmod +x harness && printf 'http://127.0.0.1:1/channels.m3u\n' > setup.txt"

# session name | options | seconds | playlist | harness script
run() {
  local name=$1 opts=$2 secs=$3 list=$4 script=$5
  echo "session $name"
  ssh -o BatchMode=yes "$PI" "cd $REMOTE && rm -rf saves shots && mkdir shots && HARNESS_OPTS='$opts' ./harness ./retrotv_libretro.so $list $secs shots '$script' >/dev/null 2>&1; mkdir -p out/$name && mv shots/*.png out/$name/ 2>/dev/null; true"
}
run main "" 36 demo.m3u "shot@1.5 b@2 shot@3 start@5 shot@6 a@8 y@9 shot@10 a@10.5 select@11 shot@12 a@12.5 up@13 up@14 shot@18.5 x@19 shot@20.2 up@22 up@23 up@24 shot@26 up@28 shot@34"
run teletext "retrotv_theme=teletext,retrotv_static_style=vhs" 13 demo.m3u "b@1.5 shot@3 start@5 shot@6 a@7 down@8 down@9 shot@11"
run phosphor "retrotv_theme=phosphor" 8 demo.m3u "b@1.5 shot@3 start@5 shot@6"
run p240 "retrotv_resolution=240" 8 demo.m3u "b@1.5 shot@3 start@5 shot@6"
run setup "" 5 setup.txt "shot@1 b@2 shot@3.5"

rm -rf build/demo-shots && mkdir -p build/demo-shots "$OUT"
scp -q -r -o BatchMode=yes "$PI:$REMOTE/out/*" build/demo-shots/
ssh -o BatchMode=yes "$PI" "rm -rf $REMOTE"

# name the frames; 240p frames are shown 2x tall, as a CRT would
pick() { ffmpeg -nostdin -v error -y -i "build/demo-shots/$1" -compression_level 100 "$OUT/$2.png"; }   # harness PNGs are uncompressed
pick main/shot_001.5.png welcome
pick main/shot_003.0.png banner
pick main/shot_006.0.png guide
pick main/shot_010.0.png menu
pick main/shot_012.0.png channel-list
pick main/shot_018.5.png fit
pick main/shot_020.2.png zoom
pick main/shot_026.0.png tuning
pick main/shot_034.0.png no-signal
pick teletext/shot_003.0.png theme-teletext
pick teletext/shot_006.0.png guide-teletext
pick teletext/shot_011.0.png tuning-vhs
pick phosphor/shot_003.0.png theme-phosphor
pick phosphor/shot_006.0.png guide-phosphor
pick setup/shot_001.0.png setup
pick setup/shot_003.5.png test-card
for f in banner guide; do
  ffmpeg -nostdin -v error -y -i "build/demo-shots/p240/shot_00$([[ $f == banner ]] && echo 3.0 || echo 6.0).png" -vf scale=640:480:flags=neighbor -compression_level 100 "$OUT/$f-240p.png"
done
ls -la "$OUT"
