#!/bin/bash
# Does Linux also use MCU_TIMER0 (0x04800000), the timer the R5 firmware uses?
echo "== device tree nodes at 4800000"
for n in $(find /proc/device-tree -type d -name '*4800000*' 2>/dev/null); do
  echo "$n: status=$(tr -d '\0' < $n/status 2>/dev/null || echo '(none = enabled)') compatible=$(tr '\0' ' ' < $n/compatible 2>/dev/null)"
done
echo "== /proc/iomem"; grep -i '04800000' /proc/iomem || echo "(not claimed by a Linux driver)"
echo "== timer interrupts in Linux"; grep -iE 'timer|dmtimer' /proc/interrupts | head
