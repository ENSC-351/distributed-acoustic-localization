#!/bin/bash
# Angle-accuracy test for one node: you tap at marked angles when told to; the raw
# samples and the true angle of every tap are saved for offline analysis
# (dsp/tools/analyze_marks.py).
#   sudo ./record_marks.sh [taps per mark] [marks...]
#   default: 5 taps at -90 -60 -30 0 30 60 90 (degrees; negative = mic A side,
#            0 = straight out of the pair, +-90 = in line with the mics)
# Put tape at about 1 m from the middle of the mic pair at each angle first.
# Needs r5cap_ring_read and the R5 files in this folder. Writes
# marks_<time>.wav/.txt/.labels next to this script.
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
cd "$(dirname "$0")" || exit 1
# Output goes next to this script, unless root can't write here (NFS share with
# root_squash): then to /tmp. Set OUTDIR to choose.
OUTDIR=${OUTDIR:-$PWD}
touch "$OUTDIR/.w$$" 2>/dev/null && rm -f "$OUTDIR/.w$$" || OUTDIR=/tmp
TAPS=${1:-5}
shift 2>/dev/null
MARKS=${*:--90 -60 -30 0 30 60 90}
NAME=$OUTDIR/marks_$(date +%H%M%S)
GAP=2        # seconds between taps
MOVE=4       # seconds to move to the next mark
[ -x ./r5cap_ring_read ] || { echo "r5cap_ring_read missing here"; exit 1; }
./r5cap_ctl status | grep -q 'running rate' || ./r5_start.sh || exit 1

NMARKS=$(echo $MARKS | wc -w)
SECS=$((3 + NMARKS * (MOVE + TAPS * GAP) + 2))
./r5cap_ring_read $SECS > $NAME.wav 2> $NAME.txt &
REC=$!
sleep 0.5
kill -0 $REC 2>/dev/null || { echo "recorder failed to start:"; cat $NAME.txt; exit 1; }
T0=$(date +%s.%N)
now() { awk -v a="$(date +%s.%N)" -v b="$T0" 'BEGIN{printf "%.3f", a-b+0.5}'; }   # seconds into the recording
echo "# mark_deg prompt_time_s (seconds into the recording)" > $NAME.labels
echo "Recording $SECS s. Quiet for 3 s..."
sleep 3
for m in $MARKS; do
  echo "------ move to the ${m} deg mark ------"
  sleep $MOVE
  for i in $(seq 1 $TAPS); do
    echo "   TAP at ${m} deg  [$i/$TAPS]"
    echo "$m $(now)" >> $NAME.labels
    sleep $GAP
  done
done
echo "done tapping, quiet until the end..."
wait $REC
chown --reference=. $NAME.* 2>/dev/null
echo "saved: $NAME.wav ($(stat -c %s $NAME.wav) bytes), $NAME.txt, $NAME.labels"
grep -E 'blocks_skipped|late|adc_errors' $NAME.txt
