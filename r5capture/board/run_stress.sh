#!/bin/bash
# T0.4 under heavy contention. The real build must show 0 corrupt every run;
# each mut_* build is deliberately broken and SHOULD show corrupt blocks.
cd "$(dirname "$0")"
export T04_READERS=${T04_READERS:-12} T04_RING=${T04_RING:-2} T04_FRAMES=${T04_FRAMES:-64}
echo "readers=$T04_READERS ring=$T04_RING frames=$T04_FRAMES on $(nproc) CPUs"
for b in test_ring mut_no_recheck mut_no_writing_mark mut_no_barriers; do
  [ -x "./$b" ] || { echo "== $b (not built, skipped)"; continue; }
  echo "== $b"
  for i in 1 2 3; do ./$b | grep -E 'total corrupt|test_ring:'; done
done
