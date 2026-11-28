#!/bin/bash
# Phase 1 (T1.1, T1.2): load our heartbeat firmware on the MCU R5F and restart it N times.
# Run on the BeagleY-AI:   sudo ./heartbeat_cycle.sh <firmware.elf> <r5_heartbeat_read> [cycles]
# The firmware is copied under a test name, so nothing loads it at boot.
set -u
ELF=$1; READER=$2; CYCLES=${3:-10}
FW=r5test-heartbeat.elf
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
RP=
for r in /sys/class/remoteproc/*; do
  [ "$(cat "$r/name")" = "79000000.r5f" ] && RP=$r
done
[ -n "$RP" ] || { echo "MCU R5F not found"; exit 1; }
# Never start a running core: remoteproc would only count another boot
[ "$(cat "$RP/state")" = offline ] || { echo "R5 is $(cat "$RP/state"), not offline"; exit 1; }
cp "$ELF" /lib/firmware/$FW && echo $FW > "$RP/firmware" || exit 1

ok=0
for i in $(seq 1 "$CYCLES"); do
  echo start > "$RP/state" || { echo "cycle $i: start FAILED"; break; }
  sleep 1
  out=$("$READER"); beat=$?
  echo "cycle $i: state=$(cat "$RP/state") $(echo "$out" | tr '\n' ' ')"
  if ! echo stop > "$RP/state"; then echo "cycle $i: stop FAILED"; break; fi
  state=$(cat "$RP/state")
  [ "$state" = offline ] || { echo "cycle $i: still $state after stop"; break; }
  if "$READER" > /dev/null; then echo "cycle $i: still beating after stop"; break; fi
  [ $beat = 0 ] && ok=$((ok + 1))
done
echo "$ok/$CYCLES cycles started, beat, and stopped; final state=$(cat "$RP/state")"
