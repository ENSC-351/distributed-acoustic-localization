#!/bin/bash
# Build the node for Linux (x86) and run two nodes against the fake master.
set -u
cd "$(dirname "$0")"
gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -pthread -Icommon/include -Inode/include \
    common/src/*.c node/src/*.c -o node-host || exit 1
echo "built node-host: $(file -b node-host | cut -d, -f1-2)"

PORT=35177
python3 -u fake_master.py --port $PORT --bind 127.0.0.1 > master.log 2>&1 &
MASTER=$!
sleep 1
./node-host 1 127.0.0.1 $PORT > node1.log 2>&1 &
N1=$!
sleep 0.3
./node-host 2 127.0.0.1 $PORT > node2.log 2>&1 &
N2=$!
sleep 7
kill -INT $N2; sleep 0.5
kill -INT $N1; sleep 0.5
kill $MASTER; sleep 0.5   # SIGTERM: background jobs in scripts ignore SIGINT
wait 2>/dev/null
echo "===== fake master log"; cat master.log
echo "===== node 1 log"; cat node1.log
echo "===== node 2 log"; cat node2.log
