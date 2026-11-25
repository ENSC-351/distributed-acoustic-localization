#!/bin/bash
# Load one firmware onto the MCU R5F (leaving it running) and collect every check.
# Usage on the BeagleY-AI, right after a reboot:
#   sudo ./phase1_diag.sh r5capture-hb.elf | tee diag.txt
# (tee runs as you: root can't write to the NFS share.) Reboot before the next run.
cd "$(dirname "$0")"
ELF=${1:?usage: sudo ./phase1_diag.sh <firmware.elf>}
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ "$(cat /sys/class/remoteproc/remoteproc2/state)" = offline ] || { echo "R5 not offline: sudo reboot first"; exit 1; }
{
  echo "##### $ELF"
  NO_STOP=1 SRC=$PWD/$ELF FW=r5test-diag.elf ./phase1_load_ti.sh 2>&1 | grep -E 'state=|is now up|creating channel|Zephyr version|error|fail'
  sleep 2
  ./r5shell.sh "r5timer" "kernel uptime" "r5cap"
  sleep 2
  ./r5shell.sh "r5timer" "kernel thread list"
  ./r5cap_hb_read 2
  ./linux_timer_check.sh
} 2>&1
