#!/bin/bash
# Records ~44 s of raw R5 samples while telling you when to clap, for offline
# analysis of real impacts (noise level, drift, delays, detector thresholds).
#   sudo ./record_claps.sh [name]
# Needs the R5 sampling already (node_r5_test.sh or r5_start.sh) and
# r5cap_ring_read in this folder. Writes /tmp/<name>.wav and /tmp/<name>.txt.
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
cd "$(dirname "$0")" || exit 1
NAME=${1:-claps}
[ -x ./r5cap_ring_read ] || { echo "r5cap_ring_read missing here"; exit 1; }
./r5cap_ctl status | grep -q 'running rate' || ./r5_start.sh || exit 1

# Timeline: 5 s silence, then per side: 3 s to get ready + 3 claps 3 s apart, then 3 s quiet
GAP=3
SECS=$((5 + 3 * (GAP + 3 * GAP) + 3))
./r5cap_ring_read $SECS > /tmp/$NAME.wav 2> /tmp/$NAME.txt &
REC=$!
T0=$(date +%s.%N)
say() { printf "%5.1f s  %s\n" "$(awk -v a="$(date +%s.%N)" -v b="$T0" 'BEGIN{print a-b}')" "$1"; }
sleep 0.3
say "SILENCE: don't move, don't talk ($SECS s recording)"
sleep 5
for side in "mic A side (channel 0)" "mic B side (channel 1)" "straight in front, between the mics"; do
  say "get ready: next claps on the $side"
  sleep $GAP
  for i in 1 2 3; do say ">>> CLAP now, $side  [$i/3]"; sleep $GAP; done
done
say "quiet until the end..."
wait $REC
echo "done: /tmp/$NAME.wav ($(stat -c %s /tmp/$NAME.wav) bytes), /tmp/$NAME.txt"
grep -E 'frames|rate|late|error|offset' /tmp/$NAME.txt | head -8
