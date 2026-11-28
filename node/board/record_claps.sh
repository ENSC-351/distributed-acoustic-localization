#!/bin/bash
# Records ~44 s of raw R5 samples while telling you when to clap, for offline
# analysis of real impacts (noise level, drift, delays, detector thresholds).
#   sudo ./record_claps.sh [name]
# Needs the R5 sampling already (node_r5_test.sh or r5_start.sh) and
# r5cap_ring_read in this folder. Writes <name>_<time>.wav/.txt in this folder
# (not /tmp: root may not overwrite another user's file there).
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
cd "$(dirname "$0")" || exit 1
# Output goes next to this script, unless root can't write here (NFS share with
# root_squash): then to /tmp. Set OUTDIR to choose.
OUTDIR=${OUTDIR:-$PWD}
touch "$OUTDIR/.w$$" 2>/dev/null && rm -f "$OUTDIR/.w$$" || OUTDIR=/tmp
NAME=$OUTDIR/${1:-claps}_$(date +%H%M%S)
[ -x ./r5cap_ring_read ] || { echo "r5cap_ring_read missing here"; exit 1; }
./r5cap_ctl status | grep -q 'running rate' || ./r5_start.sh || exit 1

# Timeline: 5 s silence, then per side: 3 s to get ready + 3 claps 3 s apart, then 3 s quiet
GAP=3
SECS=$((5 + 3 * (GAP + 3 * GAP) + 3))
./r5cap_ring_read $SECS > $NAME.wav 2> $NAME.txt &
REC=$!
sleep 0.5
kill -0 $REC 2>/dev/null || { echo "recorder failed to start:"; cat $NAME.txt; exit 1; }
T0=$(date +%s.%N)
say() { printf "%5.1f s  %s\n" "$(awk -v a="$(date +%s.%N)" -v b="$T0" 'BEGIN{print a-b}')" "$1"; }
say "SILENCE: don't move, don't talk ($SECS s recording)"
sleep 5
for side in "mic A side (channel 0)" "mic B side (channel 1)" "straight in front, between the mics"; do
  say "get ready: next claps on the $side"
  sleep $GAP
  for i in 1 2 3; do say ">>> CLAP now, $side  [$i/3]"; sleep $GAP; done
done
say "quiet until the end..."
wait $REC
chown --reference=. $NAME.wav $NAME.txt 2>/dev/null   # yours, not root's
echo "done: $NAME.wav ($(stat -c %s $NAME.wav) bytes), $NAME.txt"
grep -E 'frames|rate|late|error|offset' $NAME.txt | head -8
