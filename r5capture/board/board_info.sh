#!/bin/bash
# Read-only: collects what the R5 plan needs (docs/plan.md, Open questions).
# Run on the BeagleY-AI:  sudo ./board_info.sh > board_info.txt
cd "$(dirname "$0")"
sec() { echo; echo "===== $1"; }

sec "system"
uname -a
cat /proc/device-tree/model 2>/dev/null; echo
grep -h PRETTY_NAME /etc/os-release

sec "Q2: remote processors (R5F / C7x cores)"
for r in /sys/class/remoteproc/remoteproc*; do
  [ -e "$r" ] || continue
  printf '%s  name=%s  state=%s  firmware=%s\n' "$(basename $r)" \
    "$(cat $r/name 2>/dev/null)" "$(cat $r/state 2>/dev/null)" "$(cat $r/firmware 2>/dev/null)"
done
ls -l /lib/firmware/ 2>/dev/null | grep -iE 'r5|mcu|j722s|am67|c7|ipc|zephyr' || echo "(no R5-looking firmware in /lib/firmware)"

sec "Q3: remoteproc / rpmsg kernel messages"
dmesg 2>/dev/null | grep -iE 'remoteproc|rproc|r5f|rpmsg|k3-r5|virtio' | tail -40 || echo "(dmesg needs sudo)"
lsmod 2>/dev/null | grep -iE 'rproc|rpmsg|virtio|uio' || true

sec "Q3: reserved memory regions (name: physical address, size)"
for n in /proc/device-tree/reserved-memory/*/; do
  [ -d "$n" ] || continue
  name=$(basename "$n")
  reg=$(od -An -tx1 -v "$n/reg" 2>/dev/null | tr -d ' \n')
  nomap=$([ -e "$n/no-map" ] && echo "no-map" || echo "")
  echo "$name  reg=0x$reg  $nomap"
done

sec "Q4/Q5: SPI controllers and drivers"
for s in /sys/class/spi_master/*; do
  [ -e "$s" ] || continue
  echo "$(basename $s): driver=$(basename $(readlink -f $s/device/driver) 2>/dev/null)  of_node=$(cat $s/device/of_node/name 2>/dev/null)  compatible=$(tr '\0' ' ' < $s/device/of_node/compatible 2>/dev/null)"
done
ls -l /dev/spidev* 2>/dev/null
echo "spidev bufsiz: $(cat /sys/module/spidev/parameters/bufsiz 2>/dev/null)"

sec "Q5: device tree overlays in use"
grep -iE 'fdt|overlay' /boot/firmware/extlinux/extlinux.conf 2>/dev/null || echo "(extlinux.conf not found)"

sec "Q4: pins used by SPI (needs debugfs + sudo)"
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug 2>/dev/null
grep -iE 'spi' /sys/kernel/debug/pinctrl/*/pinmux-pins 2>/dev/null | head -20 || echo "(not available)"
cat /sys/kernel/debug/gpio 2>/dev/null | grep -iE 'spi|cs|sclk|mosi|miso' | head -20

sec "CPU frequency governor (affects the current Linux sampler)"
cat /sys/devices/system/cpu/cpufreq/policy*/scaling_governor 2>/dev/null | sort | uniq -c

sec "R5 toolchains or SDKs already on the board"
ls -d /opt/* ~/zephyr* /usr/lib/gcc/arm-none-eabi 2>/dev/null || echo "(none found)"
