#!/bin/bash
# Load the BTCM experiment firmware and report whether it runs, and if not, why.
# Run on the BeagleY-AI right after a reboot:  sudo ./phase3_btcm_test.sh | tee btcm.txt
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
./mcspi_power_on.sh > /dev/null || { ./mcspi_power_on.sh; exit 1; }
[ "$(cat /sys/class/remoteproc/remoteproc2/state)" = offline ] || { echo "R5 not offline: sudo reboot first"; exit 1; }
echo "##### load r5capture-btcm.elf"
NO_STOP=1 SRC=$PWD/r5capture-btcm.elf FW=r5test-btcm.elf ./phase1_load_ti.sh 2>&1 | grep --line-buffered -E 'state=|is now up|creating channel|Zephyr version|error|fail'
sleep 3
echo "##### crash record";       ./r5cap_ctl crash
echo "##### heartbeat";          ./r5cap_hb_read 2
echo "##### control";            ./r5cap_ctl status
echo "##### if alive: profile flat out, then clean 16k"
if ./r5cap_ctl start 25000 1250000 irqoff profile > /dev/null 2>&1; then
  sleep 2; ./r5cap_ctl diag | sed '/last .* late frames/,$d'
  ./r5cap_ctl start 16000 1250000 irqoff
fi
