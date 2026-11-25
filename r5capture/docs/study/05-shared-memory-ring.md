# 5. Passing samples between cores: the shared-memory ring

Code: `include/r5cap_shared.h` (layout), `fw/capture.c` (writer), `linux/r5cap_reader.c` (reader).

## Ring, not ping-pong

**Ping-pong:** two buffers; the writer fills A while the reader reads B, then they swap.
Works only if the reader is **guaranteed** to finish B before A fills. At 20,000 frames/s
and 128-frame blocks, a buffer fills in 6.4 ms. Linux can't promise that (the scheduler
can delay a thread by ms; our DSP takes ~7 ms per estimate). If the reader is late, the
writer must either **wait** (breaks even sampling) or **overwrite** (reader gets half-old,
half-new data and doesn't know).

**Ring:** N slots (256 ≈ 1.6 s of audio). The writer never waits; the reader can fall up to
N−1 blocks behind before losing anything, and **finds out** when it does. Several readers
can read at their own pace. Ping-pong is a ring with N = 2.

Ping-pong *does* fit between two deterministic parties on one core (e.g. DMA filling one
buffer while the CPU handles the other).

## Layout

```
header (128 B) | block metadata × N (16 B each) | samples × N
```
Header: written once (magic, version, sizes, tick rate, frame period, channel offsets),
plus live counters (`state`, `writeSeq`, `lateTicks`, `adcErrors`).
Per slot: `seq` (which block it holds), `flags`, `firstTick`.

Cross-core rules baked into the header:
- **Fixed-width types and explicit padding**, plus `_Static_assert(sizeof(...) == 128)`.
  The R5 (32-bit) and A53 (64-bit) compilers could lay structs out differently; the build
  fails instead of silently disagreeing. (It caught a padding miscount.)
- **N is a power of two**: slot = `block & (N−1)` (one AND, no division), and the slot
  sequence stays continuous when the 32-bit block number wraps.

## The seqlock (how a reader knows it didn't get torn data)

Writer, publishing block n into slot n mod N:
```
meta.seq = WRITING          // "don't trust this slot"
barrier
write samples, firstTick, flags
barrier
meta.seq = n                // "slot holds block n"
barrier
header.writeSeq = n + 1     // "block n exists"
```
Reader, wanting block n:
```
s1 = meta.seq ; barrier ; copy ; barrier ; s2 = meta.seq
good only if s1 == n and s2 == n
```
If the writer lapped the reader, s1 ≠ n (already overwritten) or s2 ≠ n (overwritten
mid-copy): the reader counts the block as skipped instead of returning garbage.

Details that matter:
- **Init marks every slot WRITING.** After zeroing memory, every slot's seq is 0, which
  would look like "holds block 0".
- **Only N−1 slots are safe to read**: the slot after the newest may be mid-rewrite.
- **32-bit sequence numbers** (32-bit stores are atomic on the R5; 64-bit aren't), compared
  with unsigned subtraction so wrap is harmless. Block 0xFFFFFFFF equals the WRITING
  marker, so readers skip that one block (once every ~318 days at 20 kHz). Documented
  rather than complicating the protocol.
- **End of capture:** read `state` **before** `writeSeq`. The other order can see an old
  writeSeq and a new STOPPED and quit with blocks unread (a real bug, found by the stress test).

## Barriers, volatile, caches, aliasing

- **Barrier** (`dmb` on ARM): "everything before this is visible before anything after it."
  ARM cores may reorder memory accesses; without barriers the other core could see
  `seq = n` before the samples. x86 rarely reorders, so x86 tests can't prove barriers right.
- **`volatile`**: the compiler must actually perform every access (memory changes behind
  its back). It does **not** order accesses between cores; that's the barrier's job.
- **Caches**: the R5 and A53 caches aren't coherent. Either map uncached (Linux: `mmap`
  with `O_SYNC`; R5: cache off or MPU non-cacheable) or flush/clean before publishing.
  In our firmware the R5's data cache is off (file 6 explains why).
- **Device memory on Linux**: the uncached mapping is device memory, where `memcpy` may use
  unaligned or wide accesses that fault; the reader copies with 16-bit loads instead.
- **Strict aliasing**: accessing a `uint64_t` array through a struct pointer is undefined
  in C (GCC refused to compile it). Raw memory that holds a struct: `_Alignas(8) uint8_t[]`.

## How it was tested

- **Unit tests on a PC with a simulated R5** (fake timer, fake MCP3208): data integrity
  over 100,000 blocks, exact skip counts for a slow reader, sequence wrap, late ticks.
- **Concurrent test (T0.4):** writer thread + reader threads hammering a tiny ring;
  every returned block is checked against the known signal.
- **Mutation testing:** deliberately break the code and check the test fails.
  Removing the re-check after copy, or the WRITING mark, produced torn blocks (caught).
  Removing barriers: not provoked on x86 or the A53 (in-order core) — that doesn't prove
  barriers unnecessary; the ARM memory model requires them.
- **Stress mode:** more reader threads than CPUs (forces preemption mid-copy) and a
  2-slot ring. On the board's 3-reader default run the readers rarely got lapped, so
  two mutants went unnoticed; stress made the test trustworthy, and found the end-of-capture bug.
