#!/bin/bash
# Records 20 s of raw R5 samples while telling you when to clap, for offline
# analysis of real impacts (noise level, drift, delays, detector thresholds).
#   sudo ./record_claps.sh [name]
# Needs the R5 sampling already (node_r5_test.sh or r5_start.sh) and
# r5cap_ring_read in this folder. Writes /tmp/<name>.wav and /tmp/<name>.txt.
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
cd "$(dirname "$0")" || exit 1
NAME=${1:-claps}
[ -x ./r5cap_ring_read ] || { echo "r5cap_ring_read missing here"; exit 1; }
./r5cap_ctl status | grep -q 'running rate' || ./r5_start.sh || exit 1

./r5cap_ring_read 20 > /tmp/$NAME.wav 2> /tmp/$NAME.txt &
REC=$!
say() { printf '%5s  %s\n' "$1" "$2"; }
sleep 0.3
say "0 s" "SILENCE: don't move, don't talk"
sleep 5
for i in 1 2 3; do say "$((4 + i * 2 - 1)) s" ">>> CLAP on mic A side (channel 0)  [$i/3]"; sleep 1.6; done
sleep 0.2
for i in 1 2 3; do say "" ">>> CLAP on mic B side (channel 1)  [$i/3]"; sleep 1.6; done
sleep 0.2
for i in 1 2 3; do say "" ">>> CLAP straight in front, between the mics  [$i/3]"; sleep 1.6; done
say "" "quiet until the end..."
wait $REC
echo "done: /tmp/$NAME.wav ($(stat -c %s /tmp/$NAME.wav) bytes), /tmp/$NAME.txt"
grep -E 'frames|rate|late|error|offset' /tmp/$NAME.txt | head -8
