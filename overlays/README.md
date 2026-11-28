# Hardware SPI overlay

`k3-am67a-beagley-ai-mcu-spi0-adc.dtso` drives the ADC with the AM67A's
MCU_SPI0 controller. It replaces the course `spidev0` overlay (bit-banged). It uses the
same header pins (19, 21, 23, 24).
Tested on kernel 7.2.8: about 12,800 samples per second per mic at 1 MHz.

Based on the pin setup in
https://forum.beagleboard.org/t/how-to-use-hw-supported-spi-on-beagley-ai/41693

## Install (on the BeagleY-AI)

1. Check that the base device tree has the labels the overlay needs. Both
   files must exist; otherwise stop, as the overlay can't be applied:
   ```
   ls /proc/device-tree/__symbols__/mcu_spi0 /proc/device-tree/__symbols__/mcu_pmx0
   ```
2. Compile it (`sudo apt install device-tree-compiler` if needed), and copy it
   next to the overlay in use now. Find that directory first:
   ```
   grep -n fdtoverlays /boot/firmware/extlinux/extlinux.conf
   ls /boot/firmware/overlays | grep spidev0
   dtc -@ -I dts -O dtb -o k3-am67a-beagley-ai-mcu-spi0-adc.dtbo k3-am67a-beagley-ai-mcu-spi0-adc.dtso
   sudo cp k3-am67a-beagley-ai-mcu-spi0-adc.dtbo /boot/firmware/overlays/
   ```
3. Back up the boot config, then in the label you boot, replace only the
   `k3-am67a-beagley-ai-spidev0-fixed.dtbo` entry with the new file name
   (leave any other overlays). Reboot.
   ```
   sudo cp /boot/firmware/extlinux/extlinux.conf /boot/firmware/extlinux/extlinux.conf.bak
   sudo nano /boot/firmware/extlinux/extlinux.conf
   sudo reboot
   ```

If the board doesn't boot, put the SD card in a PC and restore the `.bak` file.

## Check

```
ls /dev/spidev*
readlink /sys/class/spi_master/spi*/device/driver     # want omap2_mcspi
sudo dmesg | grep -iE 'spi|pinctrl'
sudo ./audio_meter --spi-diag     # "real MHz" should follow "asked MHz"
sudo ./audio_meter --bench
```

If the device isn't `/dev/spidev0.0`, name it (the ADC code and the diagnostic both use this):

```
sudo ADC_SPI_DEVICE=/dev/spidev1.0 ./audio_meter --spi-diag
```

## Reserving a CPU core for sampling (required)

Add this to the end of the `append` line of the boot label in use, then reboot:

```
isolcpus=3 rcu_nocbs=3 irqaffinity=0-2 spidev.bufsiz=65536
```

Capture finds the reserved core by itself (`--bench` prints `reserved CPU 3`)
and refuses to start without one. Linux still makes small gaps between blocks, and a pause in
the middle of a block is possible (flagged `timingDisturbed`).
