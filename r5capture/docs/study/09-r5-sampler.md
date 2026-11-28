# 9. The R5 sampler: how it works

Code: `zephyr_app/src/` (firmware), `fw/` (portable core), `linux/` (Linux tools).
Result (2026-11-25, Madden's board): **24,000 samples/s per mic, 0 late frames**.

## The big picture

```
 R5 (MCU R5F, Zephyr 4.5.0)                              Linux (A53)
 ┌──────────────────────────────────────────┐           ┌───────────────────────────┐
 │ capture thread (never yields, IRQs off)  │           │ r5cap_ctl  start/stop/diag│
 │  every frame (25 MHz timer, busy-wait):  │  control  │      │ writes command     │
 │    CAPTURE_tick():                       │◄──────────┤      ▼                    │
 │      MCP3208 ch0 ─┐ MCU_SPI0, one 19-bit │ (shared   │ r5cap_ring_read → WAV     │
 │      MCP3208 ch1 ─┘ word per conversion  │  memory)  │ r5cap_status             │
 │      store frame in ring slot            │──────────►│ (later: AudioCapture      │
 │  every 128 frames: publish block,        │   ring    │  backend → detector →     │
 │    check for a command                   │           │  estimator → networking)  │
 └──────────────────────────────────────────┘           └───────────────────────────┘
```

## Talking to the ADC: three ways, measured

| Method | Per conversion | Clean rate per mic | Why |
|---|---|---|---|
| R5 bit-banging the GPIO pins | ~100 µs | ~4,000 | ~6 register accesses per clock bit, each ~0.7 µs from the R5 |
| R5 + **MCU_SPI0 hardware** (final) | ~20 µs | **24,000** | The controller shifts the bits; ~5 register accesses per conversion |
| Linux bit-banging (`spi-gpio`), for reference | ~28 µs | 17,177 (with gaps) | Software clock ~0.68 MHz on our kernel |

**McSPI setup** (`hal_r5_mcspi.c`): single-channel controller mode; chip select asserted
by software (`FORCE` bit) around each word; mode 0; 19-bit words; transmit on D0 (MOSI),
receive on D1 (MISO), as Joey's overlay wires them. Reference clock **48 MHz** (measured:
shift times at two dividers solved for the clock), so the divider gives 1.0 MHz or ~1.23 MHz.

**19-bit trick:** fw/mcp3208.c's 3-byte command starts with 5 zero bits. The ADC ignores
clocks before the start bit, so the HAL sends only the low 19 bits as one word: same result
bits, 21 % fewer clocks.

**Who owns the pins:** Linux's device-tree overlay muxes the pads to MCU_SPI0 and powers
the controller (`power/control = on` keeps it from runtime-suspending). The R5 then programs
its registers directly. Linux must not use the spidev device at the same time.

## Frame timing

- The capture loop busy-waits on the 25 MHz system timer for each frame's scheduled time
  (`next += period`), then calls `CAPTURE_tick()`.
- **Lateness is measured against the schedule**, not against the previous frame. The first
  version compared consecutive frames, so a loop that was steadily slightly too slow just
  ran at a lower rate and reported 0 late frames (it claimed "21,000 clean" while really
  delivering ~18,900). Now any frame starting more than half a period late, or any skipped
  slot, is counted (`CAPTURE_markLate()`) and flags its block.
- **Interrupts off while sampling** (`r5cap_ctl start ... irqoff`). With them on, a 50 Hz
  interrupt (late frames exactly every 20.0 ms, ~32-42 µs each) disturbed sampling. Nothing
  in the loop needs interrupts: commands are polled from shared memory once per block.
- **Heartbeat paused** while sampling (it woke 10x/s at higher priority: ~11 late frames/s).
- **No per-block yield**: the capture thread never gives up the CPU while sampling (a yield
  per block cost a context switch each time).

## Control through shared memory (not the shell)

`include/r5cap_control.h`, in the last 4 KB page of the shared region:
- **control**: Linux writes rate, SPI clock, flags, then bumps `seq`; the R5 notices (once per
  block while sampling, every 10 ms when idle), applies, writes `ackSeq`.
- **diag**: late-frame count, worst lateness, the times of the last 32 late frames, and an
  optional per-frame profile.
- **crash record**: written by the fatal-error handler (reason, PC, LR, DFSR/DFAR, IFSR/IFAR).

Why not the R5's shell: it shares the CPU with the capture loop (asking it for stats made
frames late), and a script writing to its RPMsg tty right after boot could block forever.

## Measured results (Madden's board, interrupts off, SPI ~1.23 MHz)

| Rate requested | Delivered blocks/s (expected) | Late | Worst frame start |
|---|---|---|---|
| 20,000 | 157.0 (156.2) | 0 | 9.5 µs |
| 22,000 | 172.3 (171.9) | 0 | 8.7 µs |
| 24,000 | 188.0 (187.5) | **0** | 12.0 µs (half a period: 20.8 µs) |
| 25,000 | 195.7 (195.3) | 777 | 40.0 µs |

At 16,000 the worst frame start is 1.6 µs off schedule. Raw: `docs/results/sweep_atcm_stack.txt`.

## Per channel vs total

All rates are **per channel** (frames/s; one frame = one sample of each mic). 24,000 frames/s
= 48,000 ADC conversions/s. Mic 1 is converted ~20 µs after mic 0 in every frame (constant,
measured at init into `channelOffsetTicks`, corrected by the estimator).

## How fast can the MCP3208 go? (datasheet, from memory: check DS21298)

| Supply | Max clock | Max rate, **all channels together** |
|---|---|---|
| 2.7 V | 1 MHz | 50 ksps |
| 5 V | 2 MHz | 100 ksps |
| 3.3 V (ours) | not specified; ~1.2-1.3 MHz interpolated | ~60 ksps (*estimate*) |

One converter, so the limit is shared: ~30,000 per channel with 2 mics, ~20,000 with 3.
We run ~1.23 MHz / 48 ksps: above the guaranteed 2.7 V figure, within the interpolated
3.3 V one. Past the rating nothing errors; readings just get less accurate (the sample-and-hold
gets ~1.5 clocks to settle, so less time at higher clocks; it also depends on the source
impedance driving the input). Joey saw the idle level drift 1,341 → 1,334 from 1 to 2 MHz.
**To check:** noise and DC at 1.0 vs 1.23 MHz on the same signal; if 1.23 is worse, use
1.0 MHz (~20,000 per channel, fully in spec).

## Why we stopped optimizing

~24,500 is close to what the ADC allows (~31,000 theoretical). The rest (~6 µs SPI register
work per frame) is worth ~10 % at more risk; a faster clock is out of spec. And rate is no
longer the main accuracy lever: at 24k/s, 10 cm spacing, one sample near broadside is ~5°,
interpolation does better, and spacing, reflections, noise and the missing anti-aliasing
filter matter more. Revisit with a third mic (3 conversions per frame → ~16,000 per channel).
