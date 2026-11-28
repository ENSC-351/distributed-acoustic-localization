#!/bin/bash
# EACH BOOT, before loading R5 McSPI firmware: find MCU_SPI0, check it's the
# controller the firmware expects (0x04B00000), and keep it powered/clocked so
# the R5 can use it while Linux doesn't. Exits non-zero if anything is unexpected.
#   sudo ./mcspi_power_on.sh
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
for m in /sys/class/spi_master/*; do
  dev=$(readlink -f "$m/device")
  drv=$(basename "$(readlink -f "$dev/driver" 2>/dev/null)")
  case "$dev" in *4b00000*) ;; *) continue ;; esac
  echo "MCU_SPI0: $dev (driver: $drv)"
  [ "$drv" = omap2_mcspi ] || { echo "unexpected driver '$drv'"; exit 1; }
  echo on > "$dev/power/control"
  sleep 0.2
  st=$(cat "$dev/power/runtime_status")
  echo "power/control=$(cat "$dev/power/control") runtime_status=$st"
  [ "$st" = active ] || { echo "MCU_SPI0 not active: don't load the R5 McSPI firmware"; exit 1; }
  echo "Linux SPI device(s) on it: $(ls "$m"/device/spi*/ 2>/dev/null | grep -o 'spidev[0-9.]*' | sort -u | tr '\n' ' ') (don't use them while the R5 samples)"
  exit 0
done
echo "MCU_SPI0 (4b00000) not found as an SPI controller: is the mcu-spi0-adc overlay active? (setup_mcspi_overlay.sh, then reboot)"
exit 1
