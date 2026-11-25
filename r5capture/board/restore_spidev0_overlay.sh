#!/bin/bash
# Undo setup_mcspi_overlay.sh: restore the original boot config. Then reboot.
set -e
CONF=/boot/firmware/extlinux/extlinux.conf
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -e "$CONF.r5bak" ] || { echo "no backup $CONF.r5bak"; exit 1; }
cp -v "$CONF.r5bak" "$CONF"
echo "Restored. Reboot (sudo reboot)."
