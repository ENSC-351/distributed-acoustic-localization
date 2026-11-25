#!/bin/bash
# Where does the R5 spend each frame? Profiles the McSPI capture loop.
# Run on the BeagleY-AI after a reboot (or with r5capture-mcspi.elf already running):
#   sudo ./phase3_profile.sh | tee profile.txt
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
CLK=${CLK:-1250000}
./mcspi_power_on.sh > /dev/null || { ./mcspi_power_on.sh; exit 1; }
RP=/sys/class/remoteproc/remoteproc2
if ! { [ "$(cat $RP/state)" = running ] && [ "$(cat $RP/firmware)" = r5test-mcspi.elf ] && ./r5cap_ctl diag > /dev/null 2>&1; }; then
  [ "$(cat $RP/state)" = offline ] || { echo "R5 runs other firmware: sudo reboot first"; exit 1; }
  echo "##### loading r5capture-mcspi.elf"
  NO_STOP=1 SRC=$PWD/r5capture-mcspi.elf FW=r5test-mcspi.elf ./phase1_load_ti.sh 2>&1 | grep --line-buffered -E 'state=|is now up|Zephyr version|error|fail'
  sleep 2
fi
for rate in 25000 16000; do
  echo "##### profile at $rate frames/s requested (SPI $CLK Hz, interrupts off)"
  ./r5cap_ctl start $rate $CLK irqoff profile
  sleep 2
  ./r5cap_ctl diag | sed '/last .* late frames/,$d'
done
echo "##### leaving the R5 sampling cleanly at 16000 frames/s"
./r5cap_ctl start 16000 $CLK irqoff
