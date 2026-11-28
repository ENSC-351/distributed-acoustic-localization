#!/bin/bash
# Load the R5 firmware if needed and start sampling: sudo ./r5_start.sh [rate] [spi_hz]
# Run from a folder holding board/*.sh and prebuilt/*. Needs the mcu-spi0-adc overlay.
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
cd "$(dirname "$0")" || exit 1
RP=/sys/class/remoteproc/remoteproc2
[ "$(cat $RP/name)" = 79000000.r5f ] || { echo "remoteproc2 is not the MCU R5"; exit 1; }
./mcspi_power_on.sh || exit 1
case "$(cat $RP/state)" in
  offline)
    cp r5capture-mcspi.elf /lib/firmware/r5test-mcspi.elf && echo r5test-mcspi.elf > $RP/firmware &&
      echo start > $RP/state || { echo "R5 start failed (see dmesg); try a reboot"; exit 1; }
    sleep 3 ;;
  running) echo "R5 already running: not reloading" ;;   # never write 'start' to a running core
  *) echo "R5 is $(cat $RP/state)"; exit 1 ;;
esac
./r5cap_ctl start "${1:-20000}" "${2:-1250000}" irqoff
