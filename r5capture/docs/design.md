# Design

## Overview

```
 ┌──────────────── R5F core (our firmware, nothing else running) ────────────────┐
 │ hardware timer, one tick per frame (e.g. 20 kHz = every 50 us)                 │
 │   tick → for each mic: CS low, 19-clock conversion, CS high                    │
 │        → store the samples in the current ring slot                            │
 │        → every blockFrames frames: publish the block (seq number + timestamp)  │
 └──────────────────────────────────┬──────────────────────────────────────────────┘
                                    │ shared DDR region, mapped uncached
                                    │ (header + per-slot metadata + samples)
 ┌──────────────────────────────────▼──────────────────────────────────────────────┐
 │ Linux                                                                           │
 │   audio/audio_capture.c, new backend: copy blocks out of the ring               │
 │   ── same AudioCapture_* API ──► DSP thread (detector → estimator) │ meter │ ... │
 └─────────────────────────────────────────────────────────────────────────────────┘
```

Only the backend inside `audio_capture.c` changes. `AudioCapture_readBlock()`
and `AudioCapture_BlockInfo` stay the same, so the DSP, `audio_level` and
`audio_meter` work unchanged. A build or runtime option picks the backend, so
the current Linux sampler stays available as a fallback.

## Why it is faster: time per conversion

The MCP3208 needs 19 clock cycles per conversion and chip select (CS) must go
high between conversions. Everything else is overhead.

| Cost per conversion | Linux today (measured by Joey) | R5 + hardware SPI (estimate) |
|---|---|---|
| 19 clocks | ~27 us (bit-banged, ~0.7 MHz) | 19 us at 1 MHz; 15 us at 1.25 MHz |
| CS toggle | ~13 us (includes a fixed 10 us delay in the Linux SPI core) | < 1 us (register write) |
| Driver work | ~10 us (spi-gpio goes through gpiolib for every clock edge) | ~0 (SPI hardware shifts the bits) |
| **Total** | **~50 us → ~9,900 frames/s** | **~20 us → ~25,000 frames/s max** |

Run below the maximum to leave margin: **20,000 frames/s (50 us frames) at a
1 MHz SPI clock** is the first target. 1 MHz is the MCP3208's guaranteed clock
at 2.7 V; 1.25 MHz at 3.3 V is an estimate from the datasheet's range and must
be tested before relying on it.

**DMA is not needed.** Once the overhead is gone the CPU is idle most of each
frame; the remaining time is the ADC's own 19 clocks, which DMA can't shorten.
The R5 can poll the SPI FIFO or take one interrupt per frame. Revisit only if
profiling shows the R5 is busy.

## Firmware structure

```
fw/
  mcp3208.c/.h     command encoding / result decoding (portable, unit-tested)
  capture.c/.h     capture core: capture_init(), capture_tick(), late-tick detection,
                   ring publishing (portable, unit-tested on a PC)
  r5_hal.h         what the core needs from the hardware:
                     r5hal_spiTransfer(tx, rx, len)  - one CS-framed transfer
                     r5hal_nowTicks()                - free-running timer
                     r5hal_tickHz()
  hal_<platform>.c hardware layer: timer interrupt, McSPI (or GPIO) driver, pinmux,
                   memory map, resource table, boot. Depends on the toolchain.
```

Everything above the HAL is plain C11 and is tested on a PC with a simulated
HAL before it ever runs on the R5.

Per frame tick (`capture_tick()`):
1. Read the timer. If more than 1.5 frame periods passed since the last tick,
   count the missed frames (`lateTicks`) and flag the block `R5CAP_BLOCK_LATE_TICK`.
2. Convert each configured channel in order and store the 12-bit results.
3. On frame 0 of the first block, record each channel's start time relative to
   mic 0 → `channelOffsetTicks[]` (this is the inter-mic skew the estimator corrects).
4. After the last frame of a block, publish it (below).

## Shared-memory ring

Defined in [`include/r5cap_shared.h`](../include/r5cap_shared.h). Layout:

```
header (128 B) | block meta × ringBlocks (16 B each) | samples × ringBlocks
```

- **One writer (R5), any number of readers (Linux).** The writer never waits.
- **Seqlock per slot.** Publishing block *n* in slot *n mod ringBlocks*:
  `meta.seq = WRITING` → barrier → write samples, `firstTick`, `flags` → barrier →
  `meta.seq = n` → barrier → `header.writeSeq = n + 1`.
  A reader copies a slot only if `meta.seq == n` both before and after the copy;
  otherwise the slot was overwritten and the reader counts the block as skipped.
- **32-bit sequence numbers** (the R5 is a 32-bit core, so 32-bit stores are
  atomic; 64-bit ones are not). They wrap after ~318 days at 20 kHz / 128-frame
  blocks; all comparisons use unsigned subtraction anyway, so a wrap is harmless.
- **Uncached on both sides.** The R5 and A53 caches are not coherent. The R5's
  MPU marks the region non-cacheable; Linux maps it with `O_SYNC`. Barriers are
  `dmb sy` (A53) / `dmb` (R5).
- **Size.** 2 channels × 128 frames × 2 bytes = 512 B per block. 256 blocks
  (1.6 s at 20 kHz) is about 132 KB, small enough for the R5's reserved DDR region.

## Timestamps

The protocol needs `t_event_ns` in Linux `CLOCK_REALTIME`, but the R5 only
knows its own timer. Each block carries `firstTick` (timer ticks); Linux
converts ticks to `CLOCK_REALTIME`.

Preferred: the R5 reads the SoC's **global time counter** (the same counter
behind the A53's architected timer, readable from Linux user space as
`CNTVCT_EL0`). Then ticks are a shared timebase: Linux samples
(`CNTVCT_EL0`, `CLOCK_REALTIME`) pairs and converts exactly. **To verify:** that
the R5 can read this counter and at what frequency.

Fallback: the R5 uses its own timer and Linux estimates the mapping from
(`writeSeq` change, `CLOCK_MONOTONIC`) observations, giving accuracy of
roughly the polling interval. That is still far better than the ~1 ms clock
sync between nodes, which is what actually limits cross-node timestamps.

## Pins and ownership

- The R5 must **own the MCP3208's SPI pins**. The Linux spidev overlay must be
  removed so Linux doesn't also drive them, and the pinmux must route the pins
  to the SPI controller the R5 uses.
- While the R5 owns the ADC, Linux can't read other MCP3208 channels with the
  old HAL. If anything else is wired to the ADC (e.g. a joystick), the firmware
  samples those channels too and they appear as extra channels in the ring.
- If the R5 can't reach a hardware SPI controller on those pins, it bit-bangs
  them as GPIO. That still removes the CS delay and driver cost; the clock rate
  then depends on how fast the R5 can toggle a pin, measured in Phase 3.

## Loading and control

- Firmware is an ELF loaded by Linux `remoteproc` (stop core → set
  `firmware` → start), with a resource table describing its memory.
- Control (start, stop, rate) can begin as fields in the shared header that
  Linux writes and the R5 polls; RPMsg is only needed if polling proves awkward.
