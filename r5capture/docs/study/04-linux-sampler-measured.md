# 4. The Linux sampler and what it achieves

## How Joey's sampler works (audio/, hal/)

**Problem with the naive way (our assignments):** one `ioctl` per sample with `sleep`
between → spacing set by the scheduler, with jitter of tens of µs to ms.

**Joey's fix:** read a whole **block** (e.g. 16 or 128 frames × 2 channels) in **one**
SPI message. Inside a message, every conversion takes the same time, so the SPI clock,
not software, sets the spacing.

```
sampler thread (real-time, SCHED_FIFO 50):
  loop: read block in one SPI message → time it → flag if disturbed → put in ring → wake readers
readers (any number): AudioCapture_readBlock() — each gets every block independently
```

- **Per-block timing.** Each block's frame period is measured (call time minus a
  calibrated per-call overhead). Blocks more than 5 % off the running average are flagged
  `timingDisturbed` (an interrupt paused the transfer; samples may be unevenly spaced).
- **Timestamps.** `tFirstNs` = CLOCK_REALTIME of the block's first sample, estimated from
  the time around the SPI call.
- **Gaps.** Between blocks there's a gap (the syscall), reported as `gapBeforeNs`.
- **spidev buffer.** spidev copies each message through a 4096-byte buffer, rounded up per
  transfer, so only 32 conversions (16 frames) fit by default. `spidev.bufsiz=65536` allows
  128-frame blocks. **Our board is at 4096 → 16-frame blocks.**
- **Bit-banged SPI.** The course overlay drives SPI with `spi-gpio` (software toggles the
  pins), so the requested SPI clock has no effect.

## Measured on our board (`linux/sampler_stats.c`, 30 s per run)

| Metric | Idle | 4 CPU hogs | Slow reader (2 ms/block) |
|---|---|---|---|
| Long-run rate per mic (frames ÷ time) | **17,177/s** | 17,080/s | 8,055/s delivered |
| In-block rate (sampler's estimate) | 18,126/s | 18,119/s | 18,135/s |
| Mic 1 lag | 27.6 µs | 27.6 µs | 27.6 µs |
| Gap: mean / median / p99 / max | 3.8 / 3.7 / 4.3 / 17.9 µs | 8.9 / 9.6 / 11.5 / 41.6 µs | reader skipping |
| Time not sampled | 0.40 % (lower bound) | 0.95 % | 53.7 % (reader) |
| Dropped blocks | 0 % | 0 % | 53.3 % |
| Disturbed blocks | 0.09 % | 0.09 % | 0.08 % |
| ADC errors | 0 | 0 | 0 |

## What to take from it

- **Measure on the real board.** Joey's README says ~9,900/s and ~50 µs per conversion;
  ours does 17,200/s and ~28 µs. Different CPU clock or kernel.
- **Real-time priority works.** Four CPU hogs barely moved it, because the sampler thread
  outranks normal threads. (A real-time load could still hurt.)
- **Drops come from slow readers, not the sampler.** The slow-reader run lost exactly the
  share predicted by arithmetic: it reads 1 block per ~2 ms while 1 arrives every 0.93 ms.
- **Don't trust a number its own model produced.** The sampler calibrated its per-call
  overhead as 0 µs, so per-call time is counted inside the samples rather than as gap.
  The period difference (58.0 vs ~55.2 µs) suggests up to ~45 µs per block (~5 %) really
  goes unsampled. Settle it with an independent measurement (logic analyser on chip select,
  or the R5's timer). And trust "frames ÷ elapsed time" over model-derived rates.

## So what does the R5 add?

On *our* board, Linux already avoids most per-conversion overhead, so the R5's raw-rate gain
is modest (~1.5×, needs hardware SPI). Its real advantages: continuous sampling with no
per-block gaps, a fixed rate independent of Linux load and CPU frequency, an exactly
constant skew, and freeing the A53 cores for the DSP.
