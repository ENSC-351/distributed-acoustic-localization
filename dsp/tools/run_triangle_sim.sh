#!/bin/bash
# Three replay nodes + one master on this machine, no boards: checks the whole
# chain (node DSP -> network -> master) for sounds at known positions.
#   dsp/tools/run_triangle_sim.sh <build dir> [spacing m] ["x,y x,y ..."]
# Needs numpy. Prints each node's theta error and the position error from
# intersecting the three bearings (same least squares as master/src/locate.c).
# Synthetic data only. Once the master has a real localizer + fusion, compare
# its located positions with truth.txt too.
set -e
BUILD=${1:?usage: run_triangle_sim.sh <build dir> [spacing] [sources]}
SPACING=${2:-0.065}
SOURCES=${3:-"0,0 0,0.5 -0.4,-0.2 0.4,-0.2 0.2,0.15 -0.25,0.3 0,-0.45"}
TOOLS=$(cd "$(dirname "$0")" && pwd)
PY=${PYTHON:-python3}
OUT=$(mktemp -d /tmp/triangle_sim.XXXX)
PORT=47360

$PY "$TOOLS/make_triangle_replay.py" "$OUT" --spacing "$SPACING" --sources "$SOURCES"
N=$(grep -vc '^#' "$OUT/truth.txt")
SECS=$((N + 3))

timeout -s INT $((SECS + 3)) "$BUILD/master/master" $PORT > "$OUT/master.log" 2>&1 &
MASTER=$!
sleep 0.5
for n in 1 2 3; do   # started together so their sounds reach the master at the same time
  DSP_REPLAY="$OUT/node$n.csv" DSP_MIC_SPACING_M="$SPACING" \
    timeout -s INT $SECS "$BUILD/node/node" $n 127.0.0.1 $PORT > "$OUT/node$n.log" 2>&1 &
done
wait $MASTER || true
$PY "$TOOLS/triangle_check.py" "$OUT/truth.txt" "$OUT/master.log"
echo "files: $OUT"
