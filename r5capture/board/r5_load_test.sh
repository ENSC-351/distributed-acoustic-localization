#!/bin/bash
# Load test of r5capture-mcspi.elf: quiet and with three cores writing memory.
# Copy board/*.sh and prebuilt/* to one folder on the board, then: sudo ./r5_load_test.sh 2>&1 | tee results.txt (about 7 minutes)
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
cd "$(dirname "$0")"; RP=/sys/class/remoteproc/remoteproc2
[ "$(cat $RP/name)" = 79000000.r5f ] || { echo "remoteproc2 is not the MCU R5"; exit 1; }
./mcspi_power_on.sh || exit 1
if [ "$(cat $RP/state)" = running ]; then ./r5cap_ctl stop; timeout 20 sh -c "echo stop > $RP/state"; fi
[ "$(cat $RP/state)" = offline ] || { echo "R5 is $(cat $RP/state), not offline: reboot and retry"; exit 1; }
cp r5capture-mcspi.elf /lib/firmware/r5test-mcspi.elf && echo r5test-mcspi.elf > $RP/firmware && echo start > $RP/state || { echo "R5 start FAILED: reboot and retry"; exit 1; }
sleep 3; echo "R5 $(cat $RP/state)"
g() { echo "$1" | grep -o "$2=[0-9]*" | cut -d= -f2; }
run() {  # rate mode seconds
  ./r5cap_ctl start $1 1250000 irqoff > /dev/null || { echo "ctl failed at $1 $2"; ./r5cap_ctl crash; exit 1; }
  sleep 1
  if [ $2 = load ]; then for i in 1 2 3; do timeout $(( $3 + 2 )) sh -c 'while :; do dd if=/dev/zero of=/dev/shm/r5load$$ bs=1M count=60 2>/dev/null; done' & done; sleep 1; fi
  a=$(./r5cap_status); ta=$(date +%s%N); sleep $3; b=$(./r5cap_status); tb=$(date +%s%N)
  echo "$1 $2 ${3}s rate=$(awk "BEGIN{printf \"%.0f\", ($(g "$b" writeSeq)-$(g "$a" writeSeq))*128e9/($tb-$ta)}") late=$(( $(g "$b" lateTicks)-$(g "$a" lateTicks) )) adc=$(g "$b" adcErrors) $(./r5cap_ctl diag | head -1 | cut -d' ' -f2)"
  wait; rm -f /dev/shm/r5load*
}
for r in 16000 20000 22000 24000; do run $r quiet 20; run $r load 20; done
echo "--- mic delay, 5 starts"
for i in 1 2 3 4 5; do ./r5cap_ctl start 20000 1250000 irqoff > /dev/null; sleep 0.5; ./r5cap_ring_read 0.2 2>&1 >/dev/null | grep -a ch1_offset_ticks; done
./r5cap_ring_read 12 > n.wav 2> n.txt; grep -aE 'ch1_offset_ticks|late|skipped|adc_err' n.txt
run 20000 load 180
./r5cap_ctl stop; ./r5cap_ctl crash
echo "--- Linux 19-bit mode, 5 times"
for i in 1 2 3 4 5; do ./adc_record 1 2>&1 > /dev/null | grep -aE '^ADC:|transfer_mode'; done
echo ALLDONE
