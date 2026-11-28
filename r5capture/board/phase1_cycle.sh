#!/bin/bash
# Phase 1, T1.2: does stop/start really restart the MCU R5F every time?
# Run on the BeagleY-AI:  sudo ./phase1_cycle.sh 2>&1 | tee phase1_cycle.txt
# Needs /lib/firmware/r5test-openamp.elf (from phase1_load_ti.sh). Leaves the R5 stopped.
set -u
RP=/sys/class/remoteproc/remoteproc2
FW=r5test-openamp.elf
CYCLES=${CYCLES:-10}
WAIT_AFTER_START=${WAIT_AFTER_START:-3}   # seconds for the firmware to finish setting up RPMsg
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ "$(cat $RP/name)" = "79000000.r5f" ] || { echo "remoteproc2 is not the MCU R5F"; exit 1; }

writeState() {   # prints the result of writing $1 to the state file
  if echo "$1" > $RP/state 2>/tmp/rp_err; then echo "ok"; else echo "FAILED($(sed 's/.*: //' /tmp/rp_err))"; fi
}
uptimeMs() {     # asks the Zephyr shell on the R5 for its uptime
  local tty; tty=$(ls -t /dev/ttyRPMSG* 2>/dev/null | head -1)
  [ -n "$tty" ] || { echo "no-tty"; return; }
  stty -F "$tty" raw -echo 2>/dev/null
  ( sleep 0.3; printf 'kernel uptime\r\n' > "$tty" ) &
  timeout 1.5 cat "$tty" 2>/dev/null | tr -d '\r' | grep -oE 'Uptime: [0-9]+ ms' | head -1 | grep -oE '[0-9]+' || echo "no-reply"
}

echo "start state=$(cat $RP/state) firmware=$(cat $RP/firmware)"
# Never send start to a core that isn't offline: remoteproc counts boots, and a
# start on a running core just adds 1 to the count, after which one stop no
# longer stops it. That is how an earlier run got stuck.
if [ "$(cat $RP/state)" != "offline" ]; then
  echo "R5 is not offline. Reboot the board (sudo reboot) for a clean start; not cycling."
  exit 1
fi
[ -e /lib/firmware/$FW ] || { echo "missing /lib/firmware/$FW: run phase1_load_ti.sh first"; exit 1; }
echo $FW > $RP/firmware

pass=0
for i in $(seq 1 "$CYCLES"); do
  dmesg -C
  s=$(writeState start); sleep "$WAIT_AFTER_START"
  st1=$(cat $RP/state); up=$(uptimeMs)
  t=$(writeState stop); sleep 1
  st2=$(cat $RP/state)
  restarted=no
  [ "$s" = ok ] && [ "$st1" = running ] && [ "$up" != no-reply ] && [ "$up" != no-tty ] && [ "$up" -lt $(( (WAIT_AFTER_START + 2) * 1000 )) ] && restarted=yes
  stopped=no; [ "$t" = ok ] && [ "$st2" = offline ] && stopped=yes
  [ $restarted = yes ] && [ $stopped = yes ] && pass=$((pass+1))
  echo "cycle $i: start=$s -> $st1, R5 uptime=${up}ms (fresh=$restarted) | stop=$t -> $st2"
  [ $restarted = yes ] && [ $stopped = yes ] || dmesg | grep -iE 'r5f|remoteproc|rpmsg|virtio|error|fail|busy' | sed 's/^/    dmesg: /'
  if [ "$st2" != offline ]; then
    echo "stop did not reach offline: ending the test so the next start can't stack on a running core"
    break
  fi
done
echo "T1.2: $pass/$CYCLES cycles genuinely restarted and stopped"
echo "final state=$(cat $RP/state)"
