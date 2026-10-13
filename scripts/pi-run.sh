#!/bin/sh
# Runs on the Pi (copied there by pi-test.sh): start the harness and sample
# per-thread CPU once a second into cpu.txt:
#   S <uptime seconds>
#   T <tid> <utime+stime ticks> <thread name>
# The harness prints the core's log, so run.log has everything.
# usage: pi-run.sh <seconds> <script>
cd "$(dirname "$0")"
./harness ./retrotv_libretro.so channels.m3u "$1" shots "$2" > run.log 2>&1 &
pid=$!
while kill -0 "$pid" 2>/dev/null; do
  echo "S $(cut -d' ' -f1 /proc/uptime)"
  for t in /proc/"$pid"/task/*; do
    # comm may contain spaces: take fields after the closing parenthesis
    sed -n 's/^\([0-9]*\) (\(.*\)) \(.*\)$/\1 \3 \2/p' "$t/stat" 2>/dev/null |
      awk '{ n = $0; sub(/^([^ ]+ ){51}/, "", n); print "T", $1, $13 + $14, n }'
  done
  sleep 1
done > cpu.txt
wait "$pid"
# saved state (last channel), so tests can check it before the folder is removed
find saves -type f 2>/dev/null | while read -r f; do echo "saved $f: $(tr '\n' ' ' < "$f")"; done >> run.log
