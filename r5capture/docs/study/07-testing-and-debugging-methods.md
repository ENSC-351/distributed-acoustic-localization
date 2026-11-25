# 7. Testing and debugging methods that worked

General techniques from this project, each with the example where it paid off.

## Design for testability: a hardware boundary

Split firmware into **portable logic** and a **tiny hardware layer** (`fw/r5_hal.h`: one SPI
transfer, read the timer, timer frequency). Test the logic on a PC against a **simulated**
hardware layer (`test/hal_sim.c`). When a board test fails later, the logic is already known
to be right, so look at the hardware layer.

**Inversion of control:** the capture core doesn't start a timer; the timer (ISR or a test
loop) calls `CAPTURE_tick()`. Tests can then control time exactly, e.g. inject a late tick.

**Make the fake realistic:** the simulated ADC returns a value that depends on (channel,
time of the conversion), so tests check *timing* as well as values; it fills undefined bits
with junk; failed transfers still take time; bad commands return garbage (like the real chip)
but are counted.

## Prove a test can fail (mutation testing)

Deliberately break the code and confirm the test catches it. Examples:
- Changing a decode mask from 0x0F to 0x1F: caught only by the check that puts junk in the
  null bit, not by the round-trip test. Tests for "bits we must ignore" matter.
- Removing either half of the seqlock: caught only under stress on the board.

## Make rare races common (stress testing)

A race that needs a thread to be preempted at the wrong instant almost never happens with
few threads. More threads than CPUs + a tiny buffer made it happen every run, and found a
real bug (reader quitting with blocks unread).

## Run on the target architecture

x86 keeps memory writes in order almost automatically; ARM may reorder. Barrier bugs can be
invisible on x86. Cross-compile tests and run them on the board too.

## Distrust numbers from the system's own model

The sampler's "gap" figure came from its own timing model (with a calibrated overhead of 0),
so it was probably too low. Prefer model-free numbers (frames ÷ elapsed time) and independent
instruments (logic analyser, a second clock).

## Bisect from known-good

When our first firmware failed, we changed one thing at a time from a build that worked
(TI's sample → our build of the same sample → plus heartbeat). Diffing `.config` between the
working and broken build pointed straight at the cause.

## Read the hardware's own state

When software says "sleeping forever", dump the hardware registers from the core itself
(`r5timer` shell command). `TCLR=0x3` vs `0x43` proved the cause in one run. Add a debug
shell command early: it costs little and turns guesses into facts.

## Know what each symptom means

- Uptime moves but sleeps never end → timer counts, interrupt missing.
- Magic written, then nothing → code started, then stalled (here: cache).
- `stop` "ok" but still running → remoteproc boot counter > 1.
- Files written under `sudo` don't appear on the NFS share → root_squash (pipe to `tee` as yourself).

## Make actions reversible

Load firmware under a test name at runtime instead of rewriting boot files; scripts refuse
to act when the state is unexpected (e.g. R5 not offline) instead of stacking mistakes.
