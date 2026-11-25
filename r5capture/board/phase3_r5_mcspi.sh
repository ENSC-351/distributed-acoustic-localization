#!/bin/bash
# Load the R5 hardware-SPI (MCU_SPI0) firmware, measure the real SPI clock, and
# sweep SPI clock x frame rate to find the fastest setting with ZERO late frames.
# Run on the BeagleY-AI RIGHT AFTER A REBOOT, with the mcu-spi0-adc overlay active:
#   sudo ./phase3_r5_mcspi.sh | tee mcspi.txt
# Leaves the R5 sampling at the best setting found (ready for r5cap_ring_read).
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
CLOCKS=${CLOCKS:-"1250000"}
RATES=${RATES:-"16000 18000 19000 20000 21000 22000 23000"}
MODES=${MODES:-"irqoff"}   # interrupts on: a 50 Hz interrupt makes frames late (measured)
SETTLE_S=1
MEASURE_S=3

echo "##### 1. MCU_SPI0 powered and owned by the expected controller?"
./mcspi_power_on.sh || exit 1
RP=/sys/class/remoteproc/remoteproc2
if [ "$(cat $RP/state)" = running ] && [ "$(cat $RP/firmware)" = r5test-mcspi.elf ] && ./r5cap_ctl status > /dev/null 2>&1; then
  echo "##### 2. r5capture-mcspi.elf is already running and answering r5cap_ctl: not reloading"
  ./r5cap_ctl stop > /dev/null
else
  [ "$(cat $RP/state)" = offline ] || { echo "R5 not offline: sudo reboot first"; exit 1; }
  echo "##### 2. load r5capture-mcspi.elf (does not start sampling by itself)"
  NO_STOP=1 SRC=$PWD/r5capture-mcspi.elf FW=r5test-mcspi.elf ./phase1_load_ti.sh 2>&1 | grep --line-buffered -E 'state=|is now up|Zephyr version|error|fail'
  sleep 2
fi

echo "##### 3. real SPI clock and ADC values, measured on the R5"
./r5shell.sh "r5cap stats" | grep -E "I-cache"
for clk in $CLOCKS; do
  ./r5shell.sh "r5cap spiclk $clk" "r5cap spitest" | grep -E 'clock set|ch[01] |one frame|error'
done

echo "##### 4. sweep, controlled through shared memory (r5cap_ctl); late frames over ${MEASURE_S} s"
printf "%10s %7s %8s %9s %9s %7s %7s %12s\n" "SPI Hz" "IRQs" "rate" "blocks/s" "expected" "late" "ADCerr" "worst late"
best_rate=0; best_clk=0; best_mode=irqon
get() { echo "$1" | grep -o "$2=[0-9.]*" | cut -d= -f2; }
for clk in $CLOCKS; do
  for mode in $MODES; do
    flag=""; [ "$mode" = irqoff ] && flag=irqoff
    for rate in $RATES; do
      ./r5cap_ctl start $rate $clk $flag > /dev/null || { echo "r5cap_ctl failed"; exit 1; }
      sleep $SETTLE_S
      a=$(./r5cap_status); sleep $MEASURE_S; b=$(./r5cap_status)
      blocks=$(( $(get "$b" writeSeq) - $(get "$a" writeSeq) ))
      late=$(( $(get "$b" lateTicks) - $(get "$a" lateTicks) ))
      adc=$(( $(get "$b" adcErrors) - $(get "$a" adcErrors) ))
      worst=$(get "$(./r5cap_ctl diag | head -1)" maxOvershootUs)
      bps=$(awk "BEGIN{printf \"%.1f\", $blocks/$MEASURE_S}")
      exp=$(awk "BEGIN{printf \"%.1f\", $rate/128}")
      printf "%10s %7s %8s %9s %9s %7s %7s %10s us\n" "$clk" "$mode" "$rate" "$bps" "$exp" "$late" "$adc" "$worst"
      # Clean = no late frames, no ADC errors, the R5 really delivers the rate asked for
      # (within 1 %), and no frame starts more than half a period late
      ok=$(awk "BEGIN{d=$blocks/$MEASURE_S; e=$rate/128; h=500000/$rate; print (d>=0.99*e && d<=1.01*e && $worst<h) ? 1 : 0}")
      if [ "$late" = 0 ] && [ "$adc" = 0 ] && [ "$ok" = 1 ] && [ "$rate" -gt "$best_rate" ]; then
        best_rate=$rate; best_clk=$clk; best_mode=$mode
      fi
    done
  done
done

echo "##### 4b. when do late frames happen? (16,000 frames/s, interrupts on, 3 s)"
./r5cap_ctl start 16000 $(echo $CLOCKS | awk '{print $NF}') > /dev/null; sleep 3
./r5cap_ctl diag

echo "##### 5. result"
if [ "$best_rate" = 0 ]; then
  echo "no setting ran without late frames; leaving capture stopped"
  ./r5cap_ctl stop
  exit 1
fi
echo "fastest clean setting: $best_rate frames/s per mic at SPI $best_clk Hz, $best_mode"
flag=""; [ "$best_mode" = irqoff ] && flag=irqoff
./r5cap_ctl start $best_rate $best_clk $flag
sleep 1; ./r5cap_ctl status
echo "R5 is sampling. Record with:  sudo ./r5cap_ring_read 10 > r5taps.wav 2> r5taps.txt"
