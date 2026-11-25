# R5 sampling the ADC: test it on your board with the mics (for Joey)

Branch `madden/r5-capture`. Everything you need to run is prebuilt in
[`../prebuilt/`](../prebuilt/README.md); scripts are in [`../board/`](../board).

## Where it stands (2026-11-25, Madden's board, TI kernel 6.1)

- **The R5 reads the MCP3208 through MCU_SPI0** (your overlay), one 19-bit word per
  conversion, frames paced by the R5's 25 MHz timer, samples into the shared ring, read
  by Linux. 0 ADC errors, 0 ring errors. Controlled from Linux through shared memory
  (`r5cap_ctl`), so the R5's shell isn't needed while sampling.
- **Best clean setting: 24,000 frames/s per mic, SPI ~1.23 MHz, interrupts off**
  (`sudo ./r5cap_ctl start 24000 1250000 irqoff`): 0 late frames, worst frame start
  12.0 µs off schedule (half a period is 20.8 µs); 25,000 is over the edge. Comes from
  putting the per-frame variables (16k -> 20k) and the capture thread's stack (20k -> 24k)
  in the R5's ATCM. For a safety margin, 20,000-22,000 is a comfortable setting.
- **Use `irqoff`.** With interrupts on, a 50 Hz interrupt on the R5 makes a frame late
  every 20.0 ms (~32-42 µs). Measured with `r5cap_ctl diag`.
- **Ceiling ~24,500 frames/s**, close to the ADC's own limit (~31,000: 19 clocks per
  conversion at ~1.23 MHz). Asking for more doesn't go faster. Older builds reported 0 late
  frames there anyway; the current build counts slips against the schedule, and the
  sweep checks delivered blocks/s, so it won't happen again.
- Reference clock is 48 MHz; the R5's instruction cache is on (data cache off); per-frame
  variables live in the ATCM at 0x1000 (the BTCM at 0x41010000 isn't reachable from this core:
  reading it gave a synchronous external abort, caught by the crash recorder, `r5cap_ctl crash`).
- Not yet done: the analysis of a real tap recording through the R5 (that's Test B).

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

Set the R5 to the best clean setting, then tap the table ~20 cm from the mic about once
a second (or drop a marble a few times) during each recording:
```
sudo ./r5cap_ctl start 24000 1250000 irqoff               # don't trust a rate above ~24,000
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
sudo ./r5cap_ctl start 24000 1250000 irqoff   # rate (frames/s per mic), SPI clock (Hz), interrupts off
sudo ./r5cap_ctl diag                    # late frames while sampling: count, worst, time between
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

## Known issues on Madden's board

- Once, the whole board froze (no ping) while loading R5 firmware; the next load of the same
  build worked. Not reproduced; cause unknown. A second, earlier "hang" was a script waiting
  on a blocked RPMsg tty write (fixed). Keep `ping` running when testing.
- `dmesg` showed 4 `Deferred event dump:` lines ~1 s after an R5 start; source not identified
  yet (possibly the CC33xx Wi-Fi driver).
