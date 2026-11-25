#!/bin/bash
# ONE-TIME: switch the ADC pins from the course's bit-banged spidev0 overlay to
# Joey's hardware MCU_SPI0 overlay (same pins, no rewiring). Backs up the boot
# config first; undo with ./restore_spidev0_overlay.sh. Then reboot.
#   sudo ./setup_mcspi_overlay.sh
set -e
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
CONF=/boot/firmware/extlinux/extlinux.conf
OLD=k3-am67a-beagley-ai-spidev0.dtbo
NEW=k3-am67a-beagley-ai-mcu-spi0-adc.dtbo
for s in mcu_spi0 mcu_pmx0; do
  [ -e /proc/device-tree/__symbols__/$s ] || { echo "base device tree has no '$s' label: overlay can't apply. Stopping."; exit 1; }
done
echo "mcu_spi0 is $(tr -d '\0' < /proc/device-tree/__symbols__/mcu_spi0)"
grep -q "$OLD" "$CONF" || { echo "$OLD not found in $CONF (already switched?)"; grep -n fdtoverlays "$CONF"; exit 1; }
OVDIR=/boot/firmware$(grep -m1 -o "/[^ ]*/$OLD" "$CONF" | sed "s#/$OLD##")
[ -d "$OVDIR" ] || { echo "overlay directory $OVDIR not found"; exit 1; }
cp -v "$NEW" "$OVDIR/"
[ -e "$CONF.r5bak" ] || cp -v "$CONF" "$CONF.r5bak"
sed -i "s#$OLD#$NEW#" "$CONF"
echo "== boot config change:"; diff "$CONF.r5bak" "$CONF" || true
echo "Done. Reboot to use hardware SPI (sudo reboot). Undo: sudo ./restore_spidev0_overlay.sh"
