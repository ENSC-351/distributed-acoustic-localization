# Plan

Each phase ends with a pass/fail check from [test-plan.md](test-plan.md). We stop
at the first phase that fails without a workaround; Joey's Linux sampler stays
the working path the whole time.

Status (2026-11-24): Phase 0 done. Phase 1: T1.1 passes with TI's prebuilt Zephyr firmware;
T1.2 fails until our firmware answers the shutdown request. Details: [handoff-joey.md](handoff-joey.md).

| Phase | Where | Result | Blocked on |
|---|---|---|---|
| 0. Portable code | PC (course VM) | Capture core, ring, reader, all tested with a simulated R5 | nothing |
| 1. R5 bring-up | Board | Our firmware loads and runs on an R5 | course R5 toolchain/method |
| 2. Shared memory | Board | R5 writes a test pattern, Linux reads it correctly | Phase 1 |
| 3. SPI to the MCP3208 | Board | R5 reads correct ADC values | Phase 2 + SPI controller/pins |
| 4. Timer-driven streaming | Board | 20,000 frames/s, continuous, no late ticks | Phase 3 |
| 5. Signal checks | Board + mics | Correct rate, noise, and inter-mic offset on real signals | Phase 4 |
| 6. Integration | Board | `AudioCapture` uses the R5 path; meter and DSP unchanged | Phase 5 |

## Phase 0: portable code (now, on the VM)

- [x] `include/r5cap_shared.h`: ring layout and seqlock protocol.
- [ ] `fw/mcp3208.c`: command encoding and result decoding (same encoding as `hal/src/adc.c`).
- [ ] `fw/capture.c`: `capture_init()`, `capture_tick()`, late-tick detection, publishing.
- [ ] `linux/r5cap_reader.c`: open (validate magic/version), read next block, report skipped blocks.
- [ ] `test/`: simulated HAL (fake timer + fake MCP3208) and tests T0.1 to T0.6.
- [ ] Build with `-Wall -Wextra -Wpedantic -Werror` for x86 and aarch64.

Exit: all Phase 0 tests pass.

## Phase 1: R5 bring-up

1. Answer open questions 1 to 3 (below) from the course R5 lectures.
2. Build the course's minimal R5 example unchanged, load it with `remoteproc`,
   and confirm it runs (LED, log, or a counter in shared memory).
3. Replace it with our own minimal firmware built the same way.
4. Script the edit → build → copy → reload cycle so it takes under a minute.

Exit: T1.1, T1.2.

## Phase 2: shared memory

1. Pick the region: a reserved DDR carve-out the R5 already has, or a new
   `reserved-memory` node. Record its physical address in the firmware and in
   a Linux config constant.
2. Mark it non-cacheable in the R5's MPU.
3. Firmware: fill blocks with a counting pattern (no ADC) using the real ring code.
4. Linux: map with `/dev/mem` + `O_SYNC` (root), run the reader, check the pattern.

Exit: T2.1, T2.2, T2.3.

## Phase 3: SPI to the MCP3208

1. Identify the SPI controller on the MCP3208's pins (open question 4).
2. Remove the Linux spidev overlay; set the pinmux for the R5's controller.
3. HAL: `r5hal_spiTransfer()` with CS per transfer, mode 0, 1 MHz.
4. Firmware reads channel 0 slowly (e.g. 10 times a second) into the ring.

If the R5 can't use a hardware SPI controller on these pins: write a GPIO
bit-bang HAL instead and measure its clock (T3.3) before continuing.

Exit: T3.1, T3.2, T3.3.

## Phase 4: timer-driven streaming

1. HAL: hardware timer interrupt at the frame rate, calling `capture_tick()`.
2. Start at 5,000 frames/s, then 10,000, then 20,000. At each step run T4.1 to T4.4.
3. Try 1.25 MHz SPI / 25,000 frames/s only after 20,000 passes, and keep it only
   if T3.2 accuracy still holds at that clock.

Exit: T4.1 to T4.4 at 20,000 frames/s.

## Phase 5: signal checks

Exit: T5.1 to T5.3. T5.2 (same signal into both channels) proves the inter-mic
offset correction, which direction finding depends on.

## Phase 6: integration

1. Add the R5 backend to `audio/src/audio_capture.c`: `AudioCapture_readBlock()`
   copies from the ring; `framePeriodNs`, `getChannelOffsetNs()` and `tFirstNs`
   come from the header and block metadata; `blocksSkipped` from the reader;
   `timingDisturbed` = `R5CAP_BLOCK_LATE_TICK`.
2. Backend chosen at init (config field or environment variable); default
   stays Linux until T6.3 passes.
3. Run `audio_meter` and the DSP unchanged on the R5 backend.

Exit: T6.1 to T6.3.

## Open questions

Answers so far (from `board/board_info.sh` and TI's firmware on the board) are in
[handoff-joey.md](handoff-joey.md#what-the-board-told-us): **1** Zephyr (board target
`beagley_ai` J722S R5 MCU, Zephyr 3.6.99); **2** MCU R5F = remoteproc2, MAIN R5F = remoteproc4;
**3** remoteproc at runtime, MCU R5F regions at `0xA1000000` (1 MB) and `0xA1100000` (15 MB);
**4** MCU pads 0/2/3/4, likely MCU_SPI0 (to confirm for the J722S). Still open: 5, 6, 7.

| # | Question | How to find out |
|---|---|---|
| 1 | Which toolchain does the course use for the R5: Zephyr, TI MCU+ SDK, or bare metal? | Course R5 lecture / sample project |
| 2 | Which R5 cores can we use? (One runs TI's device-manager firmware and is not available.) | `for r in /sys/class/remoteproc/*; do echo $r $(cat $r/name) $(cat $r/state) $(cat $r/firmware); done` |
| 3 | How is firmware loaded, and what memory does that R5 already have reserved? | `dmesg \| grep -iE 'remoteproc\|rproc\|r5f'`; `ls /proc/device-tree/reserved-memory/` |
| 4 | Which SPI controller and SoC pins carry the MCP3208, and can the R5 drive that controller? | Board pinout for header pins 19/21/23/24 (SPI0 on the Pi-style header); `readlink /sys/class/spi_master/spi0/device/driver`; `grep -i overlay /boot/firmware/extlinux/extlinux.conf` |
| 5 | Why does the course use the bit-banged `-fixed` SPI overlay? Would hardware SPI on these pins conflict with something? | Ask the TA/instructor |
| 6 | Can the R5 read the global time counter (shared timebase), and at what frequency? | SoC technical reference manual; course material |
| 7 | Does anything else use the MCP3208 (joystick, other sensors) that the firmware must also sample? | Team wiring |

## Risks and fallbacks

| Risk | Effect | Fallback |
|---|---|---|
| R5 can't use a hardware SPI controller on the ADC pins | Lower clock than planned | GPIO bit-bang on the R5 (still removes the CS delay and driver cost); or rewire to pins that a reachable controller serves |
| Course R5 setup is hard to get working | Phases 1 to 2 take much longer | Stop after Phase 2; keep the Linux sampler with its cheap fixes (bigger `spidev.bufsiz`, `performance` governor) |
| MCP3208 inaccurate above 1 MHz at 3.3 V | Ceiling ~20,000 frames/s instead of ~25,000 | Stay at 1 MHz; that's still about 2× today |
| Cache or barrier mistakes | Rare corrupted or stale blocks | Seqlock checks (T2.3) and long soak tests catch them; map uncached |
| Timestamp mapping not exact | `t_event_ns` off by tens of µs | Acceptable: cross-node clock sync is ~1 ms anyway |
| R5 path conflicts with Joey's ongoing work | Merge pain | Same API, backend chosen at init; agree the plan with Joey before Phase 6 |

## Effort (rough)

Phase 0: 1 to 2 days. Phases 1 to 2: 1 to 4 days, depending almost entirely on
how ready the course's R5 setup is. Phase 3: 1 to 3 days (more if we have to
bit-bang). Phases 4 to 6: 2 to 4 days. These are guesses until Phase 1 is done.
