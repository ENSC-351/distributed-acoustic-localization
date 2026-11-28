# Microphone sampling: full context (2026-11-25)

Everything done so far on sampling the two mics, for whoever continues (Joey, Madden,
or their agents). Newest and authoritative; older notes: `CONTEXT.md` (history and
toolchain setup), `docs/study/` (theory), `docs/handoff-joey.md`, `../audio/CONTEXT.md`.

## Where it stands

- **The R5 samples both channels at 20,000 samples/s each, interrupts off, with no late
  frames**, quiet or with Linux hammering memory. Linux reads the samples from shared memory.
- **Not connected to the program yet:** `audio_meter` and the DSP still use the Linux
  sampler. The R5 backend in `AudioCapture` is the next step.
- A live spectrum plot works over the USB network.
- Branch `jkolsteren/feature/r5-capture` contains Madden's `madden/r5-capture` up to
  `570930c`, plus the changes below. Joey's board: kernel 7.2.8, Debian 13.

## History

| Step | Rate per mic | Result |
|---|---|---|
| Linux, bit-banged SPI (`spi-gpio`) | 9.8 kHz | ~6 % lost; real clock 0.7 MHz |
| Linux, hardware SPI (MCU_SPI0 overlay) | 12.8 kHz at 1 MHz | 4.3 % lost to the rest sleep |
| Linux, hardware SPI + reserved core | 12.8 kHz | ~0.16 % lost, max gap 33 us in 3 s (154 us in 2 min); branch `jkolsteren/feature/audio-meter` |
| R5, hot data in DRAM | 16 kHz | Clean when quiet, 17 % of frames late under memory load |
| R5, Madden's variables + stack in TCM (`570930c`) | 24 kHz quiet | Late under load from 20 kHz up |
| **R5, merged: code in TCM too** | **20 kHz (24 kHz in 20 s tests)** | **No late frames, quiet or loaded** |

## Results on Joey's board

SPI 1.25 MHz requested (1.23 MHz actual), `irqoff`. Load = three cores running `dd` into
`/dev/shm`. Madden's runs were 25 s, the merged ones 20 s.

| Rate | Madden `570930c`, loaded: late / worst start | Merged, loaded: late / worst start |
|---|---|---|
| 16,000 | 0 / 28.8 us | 0 / 0.3 us |
| 20,000 | 6,096 / 50.0 us | 0 / 0.3 us |
| 22,000 | 153,027 / 45.4 us | 0 / 4.2 us |
| 24,000 | 215,358 / 41.6 us | 0 / 7.1 us |

- 20,000 loaded for 180 s: 0 late, 0 ADC errors. No longer soak has been run.
- Stop and restart: works without a reboot on 7.2.8 (not on Madden's TI 6.1 kernel).
- Linux 19-bit mode selection: correct 5/5 with a live mic.

### Mic 0 to mic 1 delay (one mic wired to both inputs, from the audio's cross-spectrum)

| Build | Header said | Measured |
|---|---|---|
| Original DRAM build | 25.92 us | 21.07 us |
| Madden `570930c` | 19.92 us | 17.7 us |
| **Merged** | **17.52 us** | **17.45 us** |

Take the value from the ring header; it changes with the build and SPI clock.

## What the merge changed, and why

1. **Code in TCM** (`R5CAP_FAST_CODE`, `zephyr_app/r5fast.ld`). Code in DRAM competes with Linux.
2. **Peripherals are Device memory** (`icache.c`). The R5's default map makes registers below
   0x80000000 Normal memory, so writes can merge or be reordered. Possibly also why enabling
   the data cache stalled the firmware (not tested).
3. **SPI guards** (`hal_r5_mcspi.c`): MCP3208 CS-high time of 500 ns enforced; a read-back and
   timer read after CS goes low (without it, the first word never shifted out once this code
   ran from TCM); 1 ms transfer timeout.
4. **Mic delay** (`capture.c`): difference between timed two-channel and one-channel frames,
   instead of timing one cold frame.
5. **Late first frame** now flags its own block (`CAPTURE_markLate()`; has a unit test).
6. **Linux HAL** (`../hal/src/adc.c`): compares transfer modes by alternating reads and allows
   for input noise; a live mic used to make it reject the fast 19-bit mode.

## Facts worth knowing

- **TCM:** Linux's device tree for this core has atcm-enable 0, btcm-enable 1, loczrama 0. The
  only TCM (32 KB) is at address 0 and is physically the BTCM, whatever Zephyr calls it.
  0x41010000 is disabled; touching it faults and wedged the core's reset until a reboot.
- **MCU_SPI0** works on header 19/21/23/24 (pads 0x0c, 0x10, 0x08, 0x00, mux 0). Linux must keep
  it powered (`mcspi_power_on.sh`) and must not use it while the R5 samples.
- **Interrupts on:** an interrupt on the R5 makes frames late (~20 ms multiples). Use `irqoff`.
- **remoteproc:** never write `start` to a running core (Linux may count it and a single `stop`
  then won't stop it). `firmware: Device or resource busy` means it's already running.
- **Clocks:** McSPI reference 48 MHz; R5 tick 25 MHz. Zephyr's tick driver needs the
  posted-write fix (`timer_fix.c`).
- **Late detection:** a loop that is slightly too slow never shows as late against the previous
  frame, so always check the delivered rate too. 18,000 on the DRAM build passed 3 s and then
  logged 311 late frames in ~3.5 min.
- **USB network:** it can stall for minutes while the board keeps running. Wait before
  power-cycling.

## Run it

On the board, copy `board/*.sh` and `prebuilt/*` into one folder (here `~/r5test`):
```
cd ~/r5test
sudo ./r5_start.sh             # loads the firmware if needed, samples at 20,000/s
sudo ./r5cap_ctl status
sudo ./r5cap_stream            # live stream for the plot; Ctrl+C to stop
sudo ./r5cap_ctl stop          # before powering off or using the Linux sampler
```
On the PC (needs numpy, matplotlib, PySide6-Essentials or Tk; on Joey's VM use
`/mnt/r5dev/venv/bin/python`):
```
python3 r5capture/tools/live_spectrum.py     # space = pause, c = clear, --help for options
```
`Errno 111` means `r5cap_stream` isn't running. It listens only on 192.168.7.2 unless given
another address; the audio is unencrypted. One client at a time. A slow client loses blocks
but can't disturb the R5.

Test: `sudo ./r5_load_test.sh`. The board needs the `mcu-spi0-adc` overlay
(`../overlays/README.md`); `isolcpus` is only needed by the Linux sampler.

## Build

- Firmware: Zephyr 4.5.0, SDK 1.0.1, board `beagley_ai/j722s/mcu_r5f0_0`. Toolchain setup is in
  `CONTEXT.md`, with v4.5.0 for this app (on Joey's VM: `. /mnt/r5dev/env45.sh`).
  ```
  west build -p -b beagley_ai/j722s/mcu_r5f0_0 -d build/r5app r5capture/zephyr_app -- -DR5CAP_SPI=mcspi
  ```
- Unit tests: `cd r5capture/test && make test`.
- Streamer:
  ```
  aarch64-linux-gnu-gcc -std=c11 -O2 -D_DEFAULT_SOURCE -Ir5capture/include -Ir5capture/linux \
      r5capture/linux/r5cap_stream.c r5capture/linux/r5cap_reader.c -o r5cap_stream
  ```

## Not done, or not known

- **R5 backend in `AudioCapture`** (same API, chosen at init, Linux as the fallback), including
  converting R5 ticks to `CLOCK_REALTIME`. The stream and WAV tools have no wall-clock times.
- **Second mic:** with two mics, channel 1 drifted from ~512 to ~42 counts and clipped at 0, on
  both R5 and Linux. With one mic on both inputs the channels match, so suspect that mic's
  circuit or wiring. Not resolved.
- **Clipping:** loud sound near the mic hit 0 counts. Check before tap tests.
- **Real taps and angles** (T6.3) not done.
- **Anti-aliasing:** no analog low-pass filter is known to be fitted; at 20 kHz, sound above
  10 kHz aliases.
- **1.25 MHz SPI at 3.3 V** is interpolated from the datasheet (1 MHz at 2.7 V, 2 MHz at 5 V).
- **Merged build on Madden's board / kernel 6.1:** not tested.
- Madden's board froze once during an R5 load; not seen on Joey's, cause unknown.
- **Load at boot:** nothing starts the R5 automatically.
- The Linux sampler won't run without the overlay and a reserved core.
