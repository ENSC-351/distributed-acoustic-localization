# 11. The testing and debugging journey

Every significant problem from Milestone 2 to the 24k R5 sampler, in the order we hit it:
**symptom → suspicion → test → finding → fix → lesson.** Chapter 7 has the general methods;
this is the story, including the wrong turns.

## Part 1: Milestone 2 (2026-11-16)

**1. The "results" file was a crash.** `pipeline_check_output.txt` was supposed to show the
detector → estimator results, but it contained a Windows `FileNotFoundError` (the script
couldn't find `tdoa_cli`). The real results were in `results.json` from an earlier run.
→ Lesson: open the evidence before quoting it; a file name isn't a result.

**2. No numpy on the VM or the board.** No pip, no sudo password, board offline. Unpacked
Debian `.deb` packages into a scratch folder with `dpkg -x` and pointed `PYTHONPATH` at them
(VM); downloaded the exact arm64 `.deb`s apt asked for and installed them from the share
(board). → Lesson: when the network or permissions block you, move the packages yourself.

**3. Proof logs and the board clock.** The board had no time sync, so its logs said 2026-09-26.
Noted it instead of "fixing" the logs. → Lesson: never edit program output; annotate around it.

**4. Integration on the board.** Tyler's windows → estimator → Jack's node → Zur's master:
9/9 events, angles within 0.3° on clean cases; the low-SNR case wrong by 4° but flagged
(confidence 0.02). Estimates took ~7 ms on the board vs 0.8 ms on x86.
→ Lesson: always run on the target; performance differs by ~9× here.

## Part 2: The ring and its tests (Phase 0)

**5. A header padding miscount.** The 128-byte shared header was 8 bytes off; a
`_Static_assert(sizeof == 128)` failed the build. → Lesson: assert layout for cross-core structs.

**6. A header/implementation name mismatch.** The `.c` defined `Mcp3208_…`, the header declared
`MCP3208_…` / `MCP3209_decoderesult`. Compiled fine alone, failed to link with callers.
Fix + `-Wmissing-prototypes`. → Lesson: let the compiler check declarations match.

**7. Strict aliasing.** A `uint64_t` buffer accessed through a struct pointer: GCC refused.
Raw memory holding a struct = `_Alignas(8) uint8_t[]`.

**8. Does the test have teeth?** Broke the decoder mask on purpose (0x0F → 0x1F): only the
"junk bits" check caught it, not the round-trip test. Broke each half of the seqlock: the
concurrent test caught it on x86. → Lesson: mutation-test your tests.

**9. On the board, the concurrent test was too gentle.** Readers rarely got lapped (12-315
skipped of 200,000), so two of three broken builds passed. Added a stress mode (12 readers on
4 CPUs, 2-slot ring) → it caught them, and **found a real bug**: at the end of a capture a
reader could read an old `writeSeq` then a new `STOPPED` and quit with blocks unread.
Fix: read `state` before `writeSeq`, with a matching barrier on the writer side.
→ Lesson: make rare races common; and run on the real CPU (barrier bugs hide on x86).

## Part 3: Getting code onto the R5 (Phase 1)

**10. Which toolchain?** Unknown until a board survey script listed `/opt/ti-zephyr-firmware`
(prebuilt Zephyr, OpenAMP). Inspecting the ELF with `readelf`/`strings` gave the Zephyr
version, board name and memory layout before running anything.

**11. TI's firmware won't stop.** `stop` → `EBUSY`, "timeout waiting for rproc completion event":
TI's 6.1 driver waits for the firmware to acknowledge a shutdown request; the demo doesn't.
Then my script sent `start` to a core that was still running, which bumped remoteproc's boot
counter, after which every `stop` said "ok" and did nothing. Fix: reboot; scripts now refuse
to `start` a core that isn't offline. → Lesson: check the state before acting, every time.

**12. Proving a fresh boot.** "running" after a restart didn't prove a restart. Asked the R5's
shell for its uptime: ~4 s = really rebooted. → Lesson: find a signal that can't lie.

**13. The Zephyr version hunt.** 3.7: no board. 4.3.2: builds but overflows the 32 KB ATCM by
44 KB; moving RAM to DRAM with an overlay built, but the ELF had nothing at 0x0 (where the R5
starts). Searching upstream history found the two commits (DRAM default + boot springboard),
first in 4.5.0, which needs CMake ≥ 3.28 and Python ≥ 3.12 (pip CMake + uv Python in a venv).
→ Lesson: compare your ELF with a known-good one (entry point, segments) before loading it.

**14. Heartbeat: magic written, counter stuck, no RPMsg.** I had enabled
`CONFIG_CACHE_MANAGEMENT` for a cache flush; on this SoC that turns the data cache on, and
device registers became cacheable. The working sample had it off. Fix: off. Also showed my
claim "shared memory is cached" was wrong: the cache was off all along.
→ Lesson: diff `.config` between working and broken builds.

**15. Heartbeat: RPMsg fine, counter still 0.** Thread list: heartbeat "sleeping" for 21 s on a
100 ms sleep; uptime increasing. → uptime reads the counter, sleeps need the interrupt: the timer
counts but never interrupts. Register dump: `TCLR=0x3` (compare-enable missing),
`IRQSTATUS_RAW=0`, `TSICR=0x4` (posted mode). Cause: read-modify-writes of `TCLR` racing posted
writes in Zephyr's driver. Fix: set compare-enable after init (`timer_fix.c`); Joey found the
same bug independently and turned posted mode off. → Lesson: dump the hardware's own registers.

## Part 4: Measuring the Linux sampler

**16. 17,200 not 9,900.** Joey's README said ~9,900 samples/s; our board measured 17,177.
→ Lesson: measure on your own board and kernel.

**17. The model said 0 µs.** The sampler calibrated its per-call overhead as 0 µs, so its gap
figures (0.4 % unsampled) were probably too low. → Lesson: prefer model-free numbers.

## Part 5: The R5 samples the ADC

**18. Bit-banging from the R5: ~100 µs per conversion.** I had estimated 26 µs. Every register
access from the R5 costs ~0.7 µs, and bit-banging needs ~6 per clock bit. Every frame late.
→ Lesson: measure access costs before designing around them.

**19. 48 MHz, not 50.** Requested 1 MHz measured ~826 kHz. Two dividers, two shift times, two
equations → reference clock ~48.7 MHz → 48 MHz.

**20. The firmware runs from uncached DRAM.** Frame ~64 µs, of which ~32 µs SPI. Instruction
fetches went to DDR one by one (springboard marked DRAM inner non-cacheable). Fix: I-cache only
(`icache.c`). Ceiling 15.7k → 18.6k.

**21. ~11 late frames/s at every rate.** The heartbeat woke 10x/s. Fix: pause it while sampling.

**22. A wrong hypothesis: the per-block yield.** Late frames ~1 per 3 blocks; I blamed
`k_yield()`. Removing it changed nothing. → Lesson: an explanation that fits isn't proof; test it.

**23. A "hang" that was my script, then one that was my shell-priority change.** The R5 was alive
(heartbeat counted, shell answered later). The script's background tty write blocked and held
its output pipe. Replaced shell-driven control with **shared-memory control** (`r5cap_ctl`).

**24. Finding the real disturbance.** Recorded the time of every late frame: gaps of exactly
20.0, 40.0, 60.0 ms → a 50 Hz interrupt. Experiment: interrupts off → 0 late frames up to 18k.

**25. "21,000 clean" was false.** At 19k+ the R5 delivered only ~18,900 frames/s while reporting
0 late frames: the counter compared each frame with the previous one, so steady overload was
invisible. Fix: count slips against the schedule (`CAPTURE_markLate`), and the sweep checks
delivered blocks/s. → Lesson: cross-check with a number that can't lie.

**26. The profile lied a little, usefully.** Probes made frames 84 µs instead of 53. The probes
touched DRAM variables, which pointed straight at the real cost: variable accesses.

**27. The BTCM crash.** Moved hot variables to BTCM → R5 stopped after OpenAMP. Added a crash
recorder: DFSR=0x8 (synchronous external abort) at DFAR=0x4101007c (one of the moved variables),
in `captureThread`. No BTCM at that address from this core. → Lesson: make crashes report themselves.

**28. ATCM, carefully.** Zephyr's `ATCM` section would have pushed the springboard off 0x0; used
our own `.r5fast` at 0x1000 instead. 16k → 20k. Then the capture stack too: 20k → **24k**.

**29. Unexplained, still open.** One full-board freeze during an R5 load (not reproduced);
`Deferred event dump` lines in dmesg; CH0 ≈ 1,945 vs CH1 ≈ 2,391 with supposedly the same signal.

## Wrong turns worth remembering

| What I said | What was true | How we found out |
|---|---|---|
| "Shared memory will be cached on the R5" | The data cache was off; enabling it broke the firmware | Heartbeat stall + `.config` diff |
| "R5 bit-banging: ~26 µs per conversion" | ~100 µs (register accesses ~0.7 µs) | `spitest` on the board |
| "The per-block yield causes late frames" | A 50 Hz interrupt did | Removing the yield changed nothing; event timing |
| "21,000 frames/s clean" | ~18,900 delivered | blocks/s vs expected |
| "Put variables in BTCM" | BTCM unreachable from this core | Crash recorder |
| "Linux sampler ~9,900/s" (Joey's README) | 17,177/s on our board | `sampler_stats` |

## The pattern that worked

1. Build the smallest thing that can fail, and keep the last good build.
2. Make the system report on itself (counters, timestamps, crash records, register dumps).
3. Form one hypothesis, design one experiment that separates it from the alternatives.
4. Believe the measurement over the explanation; write down when you were wrong.
