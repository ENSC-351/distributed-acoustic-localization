#!/bin/bash
# Board test of the whole node pipeline on the R5 sampler, with a local master.
#   sudo ./node_r5_test.sh <test> [seconds]
# Tests (see r5capture/docs/integration-plan.md, phase F):
#   replay   synthetic bursts, no microphones (checks the build on the board)
#   silence  room quiet: count false events
#   taps     tap near the mics while it runs; prints every window and estimate
# Run from a folder holding: node, master, make_replay.py, and the R5 files
# (r5_start.sh, r5cap_ctl, mcspi_power_on.sh, r5capture-mcspi.elf).
# Settings: DSP_MIC_SPACING_M (default 0.20) and the other DSP_* variables.
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
cd "$(dirname "$0")" || exit 1
TEST=${1:-silence}
SECS=${2:-60}
PORT=47351
export DSP_MIC_SPACING_M=${DSP_MIC_SPACING_M:-0.20}
# Logs go to /tmp: root can't write to the NFS share (root_squash). Copy them off afterwards.
LOG=${LOGDIR:-/tmp}/node_${TEST}_$(date +%H%M%S)

./master $PORT > $LOG.master.txt 2>&1 &
MASTER=$!
sleep 0.5

case "$TEST" in
  replay)
    python3 make_replay.py /tmp/replay.csv 20000 "$DSP_MIC_SPACING_M" | tee $LOG.expected.txt
    DSP_REPLAY=/tmp/replay.csv DSP_VERBOSE=1 timeout -s INT "$SECS" ./node 1 127.0.0.1 $PORT 2>&1 | tee $LOG.node.txt ;;
  silence|taps)
    ./r5_start.sh || { kill $MASTER; exit 1; }
    [ "$TEST" = taps ] && export DSP_VERBOSE=1
    echo "== $TEST for $SECS s"
    timeout -s INT "$SECS" ./node 1 127.0.0.1 $PORT 2>&1 | tee $LOG.node.txt
    ./r5cap_ctl status | tee -a $LOG.node.txt ;;
  *) echo "unknown test $TEST"; kill $MASTER; exit 1 ;;
esac

sleep 1
kill -INT $MASTER 2>/dev/null; wait $MASTER 2>/dev/null
echo "== master saw:"; grep -E 'event|connected' $LOG.master.txt | tail -40
echo "== logs: $LOG.*.txt  (as yourself: cp $LOG.* /mnt/remote/myApps/integration/)"
