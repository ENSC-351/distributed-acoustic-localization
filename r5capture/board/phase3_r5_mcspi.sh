#!/bin/bash
# Load the R5 hardware-SPI (MCU_SPI0) firmware, measure the real SPI clock, and
# sweep SPI clock x frame rate to find the fastest setting with ZERO late frames.
# Run on the BeagleY-AI RIGHT AFTER A REBOOT, with the mcu-spi0-adc overlay active:
#   sudo ./phase3_r5_mcspi.sh | tee mcspi.txt
# Leaves the R5 sampling at the best setting found (ready for r5cap_ring_read).
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
CLOCKS=${CLOCKS:-"1000000 1200000 1250000"}
RATES=${RATES:-"14000 16000 17000 18000 18500 19000 19500 20000 21000"}
SETTLE_S=1
MEASURE_S=3

echo "##### 1. MCU_SPI0 powered and owned by the expected controller?"
./mcspi_power_on.sh || exit 1
[ "$(cat /sys/class/remoteproc/remoteproc2/state)" = offline ] || { echo "R5 not offline: sudo reboot first"; exit 1; }

echo "##### 2. load r5capture-mcspi.elf (does not start sampling by itself)"
NO_STOP=1 SRC=$PWD/r5capture-mcspi.elf FW=r5test-mcspi.elf ./phase1_load_ti.sh 2>&1 | grep --line-buffered -E 'state=|is now up|Zephyr version|error|fail'
sleep 2

echo "##### 3. real SPI clock and ADC values, measured on the R5"
./r5shell.sh "r5cap stats" | grep -E "I-cache"
for clk in $CLOCKS; do
  ./r5shell.sh "r5cap spiclk $clk" "r5cap spitest" | grep -E 'clock set|ch[01] |one frame|error'
done

echo "##### 4. sweep, controlled through shared memory (r5cap_ctl); late frames counted over ${MEASURE_S} s"
printf "%10s %8s %10s %10s %10s %8s\n" "SPI Hz" "rate" "blocks/s" "expected" "late" "ADCerr"
best_rate=0; best_clk=0
for clk in $CLOCKS; do
  for rate in $RATES; do
    ./r5cap_ctl start $rate $clk > /dev/null || { echo "r5cap_ctl failed"; exit 1; }
    sleep $SETTLE_S
    a=$(./r5cap_status); sleep $MEASURE_S; b=$(./r5cap_status)
    get() { echo "$1" | grep -o "$2=[0-9]*" | cut -d= -f2; }
    blocks=$(( $(get "$b" writeSeq) - $(get "$a" writeSeq) ))
    late=$(( $(get "$b" lateTicks) - $(get "$a" lateTicks) ))
    adc=$(( $(get "$b" adcErrors) - $(get "$a" adcErrors) ))
    bps=$(awk "BEGIN{printf \"%.1f\", $blocks/$MEASURE_S}")
    exp=$(awk "BEGIN{printf \"%.1f\", $rate/128}")
    printf "%10s %8s %10s %10s %10s %8s\n" "$clk" "$rate" "$bps" "$exp" "$late" "$adc"
    if [ "$late" = 0 ] && [ "$adc" = 0 ] && [ "$blocks" -gt 0 ] && [ "$rate" -gt "$best_rate" ]; then
      best_rate=$rate; best_clk=$clk
    fi
  done
done

echo "##### 5. result"
if [ "$best_rate" = 0 ]; then
  echo "no setting ran without late frames; leaving capture stopped"
  ./r5cap_ctl stop
  exit 1
fi
echo "fastest clean setting: $best_rate frames/s per mic at SPI $best_clk Hz"
./r5cap_ctl start $best_rate $best_clk
sleep 1; ./r5cap_ctl status
echo "R5 is sampling. Record with:  sudo ./r5cap_ring_read 10 > r5taps.wav 2> r5taps.txt"
