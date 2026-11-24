#!/bin/bash
cd "$(dirname "$0")"
uname -a
for t in test_mcp3208 test_hal_sim test_capture test_ring; do echo "== $t"; ./$t || echo "FAILED: $t"; done
