#!/bin/bash
# Load the R5 ADC-sampling firmware and run its first checks.
# Run on the BeagleY-AI RIGHT AFTER A REBOOT (kernel 6.1 can't stop the R5):
#   sudo ./phase3_r5_adc.sh | tee r5adc.txt
# Needs the course spidev0 overlay (bit-banged SPI pins as GPIOs), the default on
# Madden's board. Don't run adc_record / audio_meter while the R5 samples.
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ "$(cat /sys/class/remoteproc/remoteproc2/state)" = offline ] || { echo "R5 not offline: sudo reboot first"; exit 1; }
echo "##### load r5capture-adc.elf"
NO_STOP=1 SRC=$PWD/r5capture-adc.elf FW=r5test-adc.elf ./phase1_load_ti.sh 2>&1 | grep -E 'state=|is now up|Zephyr version|error|fail'
sleep 2
echo "##### capture running at the default 10,000 frames/s?"
./r5shell.sh "r5cap stats"
sleep 1
./r5shell.sh "r5cap stats"
echo "##### ADC timing and values read by the R5 (capture paused)"
./r5shell.sh "r5cap stop"
sleep 0.5
./r5shell.sh "r5cap spitest" "r5cap spitest"
./r5shell.sh "r5cap start 10000"
sleep 1
./r5shell.sh "r5cap stats" "r5hb"
echo "##### 2 s of ring data through Linux"
./r5cap_ring_read 2 2>&1 >/dev/null | grep -vE '^block,'
