# 10. Memory, caches and the optimization journey

How the R5 sampler went from ~4,000 to 24,000 samples/s per mic, and the memory theory
behind each step. Every number here was measured on the board.

## The R5's memories (and what an access costs)

| Memory | Where | Speed from the R5 | Used for |
|---|---|---|---|
| **TCM** (tightly coupled memory): ATCM 32 KB | address 0x0 | ~1 cycle, no cache needed | boot springboard, vectors, **our hot variables + capture stack** |
| BTCM 32 KB | should be 0x41010000 | **not reachable on this core** (see below) | — |
| DDR (DRAM), our carve-out | 0xA1100000-0xA2000000 | slow; **~150 ns per access when uncached** (*estimate*) | code, all normal data, shared ring |
| Peripheral registers (MMIO) | e.g. 0x04B00000 McSPI | **~0.7 µs per access** (measured) | SPI, GPIO, timer |

Caches hide DRAM's slowness, but only for memory marked cacheable and only if the cache is on.

## How the R5 decides caching: the MPU

The R5 has no MMU; its **MPU** (memory protection unit) gives each address region access
rights and a memory type. The SoC's boot springboard sets up one region: all of DRAM
(2 GB from 0x80000000), and turns on the "background region" for everything else.

Memory type bits (DRACR register): **TEX, C, B**. With TEX = 0b1xx the outer policy is TEX[1:0]
and the inner policy is C,B. The springboard used TEX=0b111, C=0, B=0: **inner
non-cacheable**. The R5's L1 caches are "inner", so DRAM was never cached, whatever the
cache enable bits said.

Two independent switches in SCTLR: **I** (instruction cache, bit 12) and **C** (data cache, bit 2).

## The steps, with what each fixed

| Step | Clean rate per mic | What changed | Why it helped |
|---|---|---|---|
| R5 bit-bangs GPIO | ~4,000 | — | ~150 register accesses per conversion × 0.7 µs |
| McSPI hardware | ~12,000 | controller shifts the bits | ~5 accesses per conversion |
| + **I-cache on** | ~12,000, ceiling 15.7k → 18.6k | MPU region → write-back; SCTLR.I = 1 | instructions came from uncached DDR one by one |
| + heartbeat paused | 12,000 (late ~11/s → 0-2) | suspend the 10 Hz thread | each wake-up exceeded half a frame period |
| + **interrupts off** | 16,000 | `irq_lock()` during sampling | a 50 Hz interrupt delayed frames by 32-42 µs |
| + **variables in ATCM** | 20,000 | `.r5fast` section at 0x1000 | every variable access was a DRAM round trip |
| + **stack in ATCM** | **24,000** | capture thread stack in `.r5fast` | every function call pushed/popped to DRAM |

Profile at 16k (µs per frame, probes add some): bookkeeping 27.5 → 15.7 (variables) → 9.4
(stack); SPI register work 12.2 → 7.8 → 6.2.

## Lessons from each step

**Instruction cache only, not data cache.** Enabling the data cache (`CONFIG_CACHE_MANAGEMENT`)
stalled the firmware: device registers outside the DRAM region fall under the background
map and became cacheable, so the timer and mailbox were read from stale cache (sleeps never
ended, RPMsg never started). The instruction cache only affects fetches, so it's safe;
`icache.c` changes the DRAM region to write-back (C=1, B=1, TEX=0b001) and sets SCTLR.I,
leaving SCTLR.C off. With the data cache off, data accesses stay uncached even in a
"cacheable" region, which is exactly what the shared ring needs.

**TCM is the fix for uncached data.** Instead of fighting the cache (MPU regions for devices,
non-cacheable shared memory, cache maintenance), put the few hot variables and the hot
thread's stack in TCM, which is always fast. 2.1 KB of 32 KB.

**The BTCM wasn't there.** First attempt used Zephyr's BTCM section at 0x41010000. The R5
booted, OpenAMP came up, then everything stopped. The crash recorder showed a **data abort,
DFSR=0x8 (synchronous external abort), DFAR=0x4101007c** = the address of one of the moved
variables, in `captureThread`. Nothing answered at that address from this core (BTCM
disabled by the kernel's defaults, or mapped elsewhere). Why boot didn't crash earlier: the
boot-time zeroing only *wrote* there, and writes to a missing address fail silently
(buffered, imprecise abort masked); the first *read* faulted precisely.

**Don't use the linker's ATCM section either.** Zephyr's `ATCM` output section starts at 0x0;
adding variables there would push the springboard off 0x0, where the R5 must start. Our own
section `.r5fast` at a fixed 0x1000 (`zephyr_app/r5fast.ld`) stays clear of the springboard
(0x0-0x4c) and of the 60-byte vector table Zephyr copies to 0x0 at boot.

**NOLOAD sections start as garbage.** remoteproc doesn't load NOLOAD sections, so `.r5fast`
holds whatever was in the memory. `fastmem.c` zeroes it at the EARLY init level, and only
variables whose initial value is 0/false/NULL may go there (`fw/r5cap_fastmem.h`).

**Placing a thread stack:** `K_THREAD_DEFINE` always puts the stack in normal RAM; instead
`Z_KERNEL_STACK_DEFINE_IN(name, size, R5CAP_FAST_DATA)` + `k_thread_create()` from an
APPLICATION-level `SYS_INIT`. Works because there's no user mode or MPU stack guard.

## Profiling: measure first, and know your probe's cost

The per-frame profile (`r5cap_ctl start ... profile`, then `diag`) timestamps parts of each
frame. Its probes read the timer and updated variables in DRAM, so the first profile showed
**84 µs per frame vs 53 µs un-instrumented**: the probes cost ~30 µs. That distortion was
itself the clue: variable accesses were the expensive part. Rule: compare the instrumented
result with an un-instrumented measurement (the sweep) before trusting the breakdown.

## Theory to remember

- **Latency, not bandwidth.** Each access waited for the bus; nothing was "busy", it was all
  waiting. That's why moving 128 bytes of variables gave +25 %.
- **The slowest thing in a hot loop sets the rate.** At the end: bit shifting ~31 µs (fixed
  by the ADC's clock), register work ~6 µs, bookkeeping ~4-9 µs per frame.
- **Know when to stop.** The remaining overhead is ~15 % of the frame; the ADC caps us at
  ~31k; and other error sources matter more than rate now.
