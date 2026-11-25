# R5 sampling the ADC: test it on your board with the mics (for Joey)

Branch `madden/r5-capture`. Everything you need to run is prebuilt in
[`../prebuilt/`](../prebuilt/README.md); scripts are in [`../board/`](../board).

## Where it stands (2026-11-25, Madden's board, TI kernel 6.1)

- **The R5 reads the MCP3208 through MCU_SPI0** (your overlay), one 19-bit word per
  conversion, frames paced by the R5's 25 MHz timer, samples into the shared ring,
  read by Linux. 0 ADC errors, 0 ring errors.
- **Measured on the R5:** reference clock 48 MHz; at 1.2 MHz SPI a conversion takes
  ~23.5 µs, a 2-mic frame ~47 µs. The loop sustained up to ~19,000 frames/s.
- **Clean (zero late frames):** 12,000 frames/s so far. Late frames above that came
  from a per-block context switch (now removed) and the heartbeat (now paused while
  sampling). **This build adds Linux control through shared memory (`r5cap_ctl`) and
  hasn't run on a board yet**; you may be first.
- What made it fast enough: hardware SPI (bit-banging from the R5 was ~100 µs per
  conversion, each register access ~0.7 µs), and turning on the R5's instruction cache
  (code runs from DRAM; data cache stays off).

## Before you start

1. **Your boot setup is fine as is:** the `mcu-spi0-adc` overlay puts the pins on MCU_SPI0.
   `isolcpus` doesn't matter for the R5.
2. **Don't run the Linux sampler (`audio_meter`, `adc_record`) while the R5 samples.**
   Both program MCU_SPI0.
3. **Mic wiring for the skew check:** one mic's preamp output to **both CH0 and CH1**.
   (On Madden's board CH0 reads ~1,890 and CH1 ~2,360 with both SPI methods, so they
   aren't getting the same signal there; please note what yours read.)
4. Copy the files into one folder on the board:
   ```
   mkdir -p ~/r5test && cp r5capture/board/*.sh r5capture/prebuilt/* ~/r5test/ && cd ~/r5test
   ```
   (`phase1_load_ti.sh` is only used as a loader here; it doesn't need TI's firmware.)

## Test A: find the fastest clean rate (~2 min)

R5 must be `offline` first (`cat /sys/class/remoteproc/remoteproc2/state`). On your
kernel `stop` works, so instead of rebooting: `echo stop | sudo tee /sys/class/remoteproc/remoteproc2/state`.
```
sudo ./phase3_r5_mcspi.sh | tee mcspi.txt
```
It checks MCU_SPI0 is at 4b00000 and powered (`mcspi_power_on.sh`), loads
`r5capture-mcspi.elf`, measures the real SPI clock (`spitest`), then sweeps SPI clock
× frame rate through shared memory and leaves the R5 sampling at the fastest setting
with zero late frames. Send back `mcspi.txt`.

## Test B: the same taps through the R5 and through Linux

With the R5 sampling (after Test A), tap the table ~20 cm from the mic about once a
second (or drop a marble a few times) during each recording:
```
sudo ./r5cap_ring_read 10 > r5taps.wav 2> r5taps.txt      # through the R5
sudo ./r5cap_ctl stop                                       # free MCU_SPI0 for Linux
sudo ADC_SPI_DEVICE=/dev/spidevX.0 ./adc_record 10 > linuxtaps.wav 2> linuxtaps.txt
```
(`mcspi_power_on.sh` prints the spidev name.) Then, on any machine with Python + numpy:
```
python3 r5capture/tools/analyze_recording.py r5taps.wav r5taps.txt --same-mic
python3 r5capture/tools/analyze_recording.py linuxtaps.wav linuxtaps.txt --same-mic
```
It reports DC, noise, clipping, rate and gaps, and per tap the dominant frequency (and the
mic spacing it allows, c/(2f)) and the CH0/CH1 delay, which must equal the measured skew.

## Handy commands

```
sudo ./r5cap_ctl status                  # what's running + ring counters
sudo ./r5cap_ctl start 16000 1200000     # rate (frames/s per mic), SPI clock (Hz)
sudo ./r5cap_ctl stop
sudo ./r5shell.sh "r5cap stats" "r5cap spitest"   # R5 shell: only while sampling is stopped
```

## What to send back

`mcspi.txt`, the four `*taps.*` files (or the two analyzer outputs), what CH0/CH1 read,
and anything odd in `sudo dmesg | grep -iE 'remoteproc|rpmsg|spi'`.

## Rebuilding the Linux tools (if you change them)

```
F="-std=c11 -O2 -Wall -Wextra -Werror -D_DEFAULT_SOURCE"
aarch64-linux-gnu-gcc $F -Ir5capture/include -Ir5capture/linux r5capture/linux/r5cap_ring_read.c r5capture/linux/r5cap_reader.c -o r5cap_ring_read
aarch64-linux-gnu-gcc $F -Ir5capture/include r5capture/linux/r5cap_ctl.c -o r5cap_ctl
aarch64-linux-gnu-gcc $F -pthread -Ihal/include r5capture/linux/adc_record.c hal/src/adc.c -o adc_record
```
