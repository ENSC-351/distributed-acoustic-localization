#!/bin/bash
# Phase 1, T1.1/T1.2 with TI's known-good Zephyr firmware on the MCU R5F (remoteproc2).
# Run on the BeagleY-AI:   sudo ./phase1_load_ti.sh 2>&1 | tee phase1_load_ti.txt
#
# Safe and reversible: the firmware is copied under a TEST name, so nothing loads
# it at boot, and /boot is not touched. Undo with:
#   sudo rm /lib/firmware/r5test-openamp.elf
set -u
RP=/sys/class/remoteproc/remoteproc2
FW=${FW:-r5test-openamp.elf}          # name under /lib/firmware (override to test other firmware)
SRC=${SRC:-/opt/ti-zephyr-firmware/j722s/mcu-r5f0_0/zephyr_openamp_rsc_table.elf}
sec() { echo; echo "===== $1"; }

[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
sec "before"
echo "name=$(cat $RP/name) state=$(cat $RP/state) firmware=$(cat $RP/firmware)"
[ "$(cat $RP/name)" = "79000000.r5f" ] || { echo "remoteproc2 is not the MCU R5F; stopping"; exit 1; }
[ "$(cat $RP/state)" = "offline" ] || { echo "R5 not offline: reboot first"; exit 1; }

sec "load and start (T1.1)"
cp -v "$SRC" /lib/firmware/$FW
modprobe rpmsg_tty 2>/dev/null && echo "rpmsg_tty module loaded" || echo "(no rpmsg_tty module)"
dmesg -C
echo $FW > $RP/firmware
echo start > $RP/state
sleep 2
echo "state=$(cat $RP/state) firmware=$(cat $RP/firmware)"
dmesg

sec "what the R5 offers Linux"
ls -l /dev/rpmsg* /dev/ttyRPMSG* 2>/dev/null || echo "(no rpmsg devices)"
ls /sys/bus/rpmsg/devices/ 2>/dev/null
for t in /sys/kernel/debug/remoteproc/remoteproc2/trace*; do [ -e "$t" ] && { echo "-- $t"; cat "$t"; }; done
if [ -e /dev/ttyRPMSG0 ]; then
  echo "-- Zephyr shell over RPMsg: sending 'kernel version'"
  stty -F /dev/ttyRPMSG0 raw -echo 2>/dev/null
  ( sleep 0.3; printf 'kernel version\r\n' > /dev/ttyRPMSG0 ) &
  timeout 2 cat /dev/ttyRPMSG0
fi

# T1.2 (repeated stop/start) is in phase1_cycle.sh, which checks every step.

if [ "${NO_STOP:-0}" = 1 ]; then
  sec "NO_STOP=1: leaving the R5 running (the stop would remove its RPMsg devices)"
  exit 0
fi
sec "leave it stopped"
echo stop > $RP/state || echo "stop FAILED"
sleep 1
echo "state=$(cat $RP/state)   (must be offline; if not, reboot before running phase1_cycle.sh)"
