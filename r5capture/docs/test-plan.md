# Test plan

Every test lists how to run it and what passing means. Record each run's
output (command, `uname -a`, firmware version, result) next to the test ID so
results can be compared later.

Targets used below: **F = 20,000 frames/s** (50 µs frames), SPI 1 MHz, 2 channels,
128-frame blocks, 256-block ring.

## Phase 0: on the PC, with a simulated R5

A simulated HAL stands in for the hardware: a fake timer the test advances,
and a fake MCP3208 that decodes each SPI command and answers with a value
computed from (channel, frame number). The capture core runs in one thread and
the reader in another, sharing an ordinary buffer.

| ID | Test | Pass |
|---|---|---|
| T0.1 | MCP3208 encoding: command bytes for channels 0 to 7; decoding of known responses | Matches the encoding in `hal/src/adc.c` for every channel |
| T0.2 | Continuous stream: reader keeps up for 100,000 blocks | Every sample equals its expected value; block numbers consecutive; `firstTick` advances by exactly `blockFrames × framePeriodTicks` |
| T0.3 | Slow reader: reader sleeps so it falls behind by more than the ring | Reader reports the exact number of skipped blocks; every block it does return is intact |
| T0.4 | Torn reads: writer runs as fast as possible while several readers copy | No reader ever returns a block whose samples don't match its block number |
| T0.5 | Late ticks: simulated timer skips ticks at known points | `lateTicks` equals the number skipped; exactly those blocks have `R5CAP_BLOCK_LATE_TICK` |
| T0.6 | Sequence wrap: start `writeSeq` near `0xFFFFFFFF` | Reader handles the wrap with no skipped or repeated blocks |

Also: everything builds with `-Wall -Wextra -Wpedantic -Werror` for x86 and aarch64.

## Phase 1: R5 bring-up

| ID | Test | Pass |
|---|---|---|
| T1.1 | Load our minimal firmware with `remoteproc` | `state` reads `running`; no errors in `dmesg`; visible sign of life (LED or counter) |
| T1.2 | Reload cycle: stop, replace firmware, start, 10 times | Works every time without rebooting the board |

## Phase 2: shared memory

| ID | Test | Pass |
|---|---|---|
| T2.1 | Header check: Linux maps the region and reads the header | `magic`, `version`, sizes as the firmware set them |
| T2.2 | Counting pattern for 10 minutes | Every block's samples match the pattern for its block number; 0 skipped blocks with a reader polling every 10 ms |
| T2.3 | Same as T2.2 with the Linux reader deliberately stalled for 5 s once a minute | Skipped blocks reported correctly; no corrupt block ever returned |

## Phase 3: SPI to the MCP3208

| ID | Test | Pass |
|---|---|---|
| T3.1 | Fixed voltages: channel tied to GND, to 3.3 V, and to a 10k/10k divider | Reads ~0, ~4095 and ~2048 (±10 counts) |
| T3.2 | Compare with the Linux path: same input read by the R5, then by Joey's `audio_meter` (Linux overlay restored) | Means agree within 10 counts; noise (standard deviation) no worse than Linux |
| T3.3 | Conversion time: R5 times 1,000 conversions with its own timer | ≤ 21 µs per conversion at 1 MHz (19 clocks + CS). If bit-banging: record the achieved clock |

## Phase 4: timer-driven streaming (at 5k, 10k, then 20k frames/s)

| ID | Test | Pass |
|---|---|---|
| T4.1 | Rate accuracy: count frames over 60 s against Linux `CLOCK_MONOTONIC` | Within 100 ppm of F |
| T4.2 | Soak: 30 minutes at F with `audio_meter`-style reader | `lateTicks` = 0, `adcErrors` = 0, 0 skipped blocks |
| T4.3 | Load: same as T4.2 while Linux runs a heavy load (e.g. `stress-ng --cpu 4`, or a full build) | Same results as T4.2: Linux load must not affect sampling |
| T4.4 | Headroom: R5 measures time spent per tick | ≤ 90% of the frame period, worst case |

## Phase 5: signal checks (real microphones)

| ID | Test | Pass |
|---|---|---|
| T5.1 | Tone: phone playing a 1 kHz tone near the mics | Spectrum peak at 1,000 Hz ± 2 Hz, computed with the header's frame rate |
| T5.2 | Inter-mic offset: one mic's preamp output wired to **both** channel 0 and channel 1; tap or play noise | Estimator with `channel_skew_s` from `channelOffsetTicks` gives \|TDOA\| < 0.05 samples. Without the correction, the measured lag equals `channelOffsetTicks` |
| T5.3 | Noise floor: mics in a quiet room, R5 path vs Linux path | R5 noise no worse than Linux |

## Phase 6: integration

| ID | Test | Pass |
|---|---|---|
| T6.1 | `audio_meter` on the R5 backend, unchanged source | Works; status line shows the new rate, `disturbed 0` |
| T6.2 | Switch backends at init, both directions | Both work; nothing else rebuilt |
| T6.3 | Impacts at known angles (e.g. −60, −30, 0, 30, 60 degrees, 10 each), Linux path vs R5 path | R5 path's mean angle error ≤ the Linux path's. This is the test that decides whether the R5 path becomes the default |
