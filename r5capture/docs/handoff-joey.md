# R5 sampling: status and handoff (for Joey)

**Branch:** `madden/r5-capture`, based on your `jkolsteren/feature/audio-meter` (`d341a96`).
Nothing in `audio/`, `hal/` or `audio_meter/` is changed. Everything new is in `r5capture/`.
**Status (2026-11-24):** Phase 0 done and tested on the VM and the board. Phase 1 half done:
the MCU R5F runs Zephyr firmware loaded from Linux; restarting it without a reboot doesn't work yet.

## Why

Your measurements (`audio/README.md`) show where the time goes on Linux: ~50 µs per
conversion, of which only the 19 SPI clocks are the ADC. The rest is spi-gpio
bit-banging (~0.7 MHz), the 10 µs chip-select delay in the Linux SPI core, and driver
work, plus gaps between blocks and "disturbed" blocks when Linux interrupts a transfer.

The plan is to move only the sampling onto the BeagleY-AI's R5F core: a hardware timer
drives the MCP3208 directly, and samples stream to Linux through shared memory.

| | Your Linux sampler today | R5 sampler (target) |
|---|---|---|
| Samples per mic | ~9,900/s on your board; **17,200/s measured on mine** (see study/04-linux-sampler-measured.md) | ~25,000/s (1 MHz SPI); maybe ~31,000 at 1.25 MHz |
| Per conversion | ~50 µs | ~20 µs |
| Stream | blocks with gaps | continuous, no gaps |
| Timing | measured per block, some disturbed | hardware timer, every frame on time |

**Your `AudioCapture_*` API stays the same.** The plan is to add the R5 as a second
backend inside `audio_capture.c`, chosen at init. The DSP, `audio_level` and
`audio_meter` shouldn't need changes, and your Linux sampler stays the fallback
until the R5 path beats it on real impacts (test T6.3).

It does **not** change the MCP3208 itself: channels are still converted one after
another (fixed, known mic-to-mic offset), and the 19 clocks per conversion set the ceiling.

## Where things are

| Path | What |
|---|---|
| [`README.md`](../README.md) | One-page overview |
| [`docs/design.md`](design.md) | Architecture, timing budget, shared-memory protocol, timestamps |
| [`docs/plan.md`](plan.md) | Phases 0 to 6, open questions, risks |
| [`docs/test-plan.md`](test-plan.md) | Tests T0.1 to T6.3 with pass criteria |
| [`include/r5cap_shared.h`](../include/r5cap_shared.h) | Shared-memory contract both cores compile |
| [`fw/`](../fw) | Portable firmware: `mcp3208.c` (encoding), `capture.c` (per-tick sampling + publishing), `r5_hal.h` (3-function hardware boundary) |
| [`linux/r5cap_reader.c`](../linux/r5cap_reader.c) | Linux reader for the ring; becomes the R5 backend in `audio_capture.c` |
| [`test/`](../test) | PC unit tests, with `hal_sim.c` faking the timer and the MCP3208 |
| [`board/`](../board) | Scripts run on the BeagleY-AI |
| [`docs/results/`](results) | Raw output from the board runs below |

## How it works (short)

- **Capture core (`fw/capture.c`)**: the timer ISR calls `CAPTURE_tick()` once per frame.
  Each tick converts every mic once (same 3-byte encoding as your `hal/src/adc.c`), stores
  the samples, flags the block if the tick came > 1.5 periods late, and publishes the block
  when full. The inter-mic offset is *measured* at init and stored in the header.
- **Hardware boundary (`fw/r5_hal.h`)**: the core only calls `R5HAL_spiTransfer()`,
  `R5HAL_nowTicks()`, `R5HAL_tickHz()`. On the PC these are faked (`test/hal_sim.c`);
  on the R5 they'll be McSPI + a timer. So the core is fully tested before it touches hardware.
- **Ring (`include/r5cap_shared.h`)**: header + per-slot metadata + samples, in a shared
  DDR region. One writer (R5), any number of readers, like your `AudioCapture` ring.
  A per-slot seqlock lets readers detect a slot being overwritten mid-copy; the writer
  never waits. Not ping-pong: Linux can lag ~1.6 s (256 blocks) before losing anything,
  instead of one block.

## Phase 0 results: portable code, tested

`cd r5capture/test && make test` (PC) and `make board` (cross-compiles to the NFS share),
then `./run_tests.sh` on the board.

| Test | VM (x86) | Board (A53, aarch64) |
|---|---|---|
| T0.1 MCP3208 encoding, all channels + all 4,096 values | pass | pass |
| Simulator self-test | pass | pass |
| Capture core: config checks, header, publishing, late ticks (T0.5), ADC errors, stop | pass | pass |
| T0.2 reader keeps up, 100,000 blocks | pass | pass |
| T0.3 slow reader: exact skip count | pass | pass |
| T0.4 concurrent readers, torn-read detection | pass | pass |
| T0.6 block-number wrap | pass | pass |

**Making sure T0.4 can actually fail.** I built deliberately broken versions and ran
them under stress (`board/run_stress.sh`: 12 readers on 4 CPUs, 2-slot ring):

| Build | VM: corrupt blocks | Board: corrupt blocks (3 runs) |
|---|---|---|
| real code | 0 | 0, 0, 0 |
| reader skips the re-check after copying | 63 | 25, 12, 14 (caught 3/3) |
| writer doesn't mark the slot "writing" first | 72 | 2, 1, 0 (caught 2/3) |
| memory barriers removed | 0 (x86 can't show it) | 0, 0, 0 (A53 didn't reorder) |

The barriers stay regardless: ARM's memory model requires them, and the real R5 → A53
path through uncached memory is only testable in Phase 2.

**Bug found by the stress test, fixed:** at the end of a capture a reader could read an
old `writeSeq` and then a new `STOPPED` state, and quit with blocks unread. The reader
now reads `state` before `writeSeq`, and `CAPTURE_stop()` has the matching barrier.

## What the board told us

From `board/board_info.sh` (raw: [`results/board_info.txt`](results/board_info.txt)).

**R5 cores:**

| remoteproc | Core | State | Usable |
|---|---|---|---|
| remoteproc2 | `79000000.r5f`, MCU-domain R5F | offline | **yes, our target** |
| remoteproc3 | `78000000.r5f` | attached, IPC-only | no: runs TI's device-manager firmware |
| remoteproc4 | `78400000.r5f`, MAIN-domain R5F | offline | yes |

**The ADC pins are in the MCU domain.** SPI0 on the header is pads 0, 2, 3, 4 of the MCU
pad controller (`4084000.pinctrl`), driven by MCU_GPIO0 lines 0/2/3/4 through `spi-gpio`
(CS0 = GPIO8, SCLK, MOSI = GPIO10, MISO = GPIO9; CS1 = GPIO7 on pad 9).
On TI's AM62x those exact MCU pads are the native pins of the hardware **MCU_SPI0**
controller. **Not yet confirmed for the J722S**: needs the datasheet pin table. If it holds,
the MCU R5F can use real hardware SPI on the pins already wired to the ADC.

**Memory for the MCU R5F** (`no-map`, so Linux can `mmap` them through `/dev/mem`):
- `0xA1000000`, 1 MB: DMA region (resource table, vrings)
- `0xA1100000`, 15 MB: firmware memory. TI's demo uses only `0xA1100000` (resource table)
  and `0xA1200000`–`0xA1213420` (~77 KB code/data). Plan: ring at `0xA1F00000`.

**Toolchain: Zephyr.** `/opt/ti-zephyr-firmware/j722s/mcu-r5f0_0/` has prebuilt firmware:
`zephyr_openamp_rsc_table.elf` (OpenAMP + RPMsg + a Zephyr shell over RPMsg), built from
Zephyr `v3.6.0-5441` ("3.6.99") with `CONFIG_BOARD_BEAGLEY_AI_J722S_R5_MCU`.
So Zephyr already supports this core as a board target; no hand-written startup code.

**Two things that affect your Linux sampler today:**
- `spidev.bufsiz` is **4096** on my board, so `AudioCapture` there runs **16-frame** blocks,
  not 128 (your README's `spidev.bufsiz=65536` fix isn't applied).
- My board uses overlay `k3-am67a-beagley-ai-spidev0.dtbo`; your README names
  `...-spidev0-fixed.dtbo`. Our boards may be set up differently.

## Phase 1 results: R5 bring-up

Scripts: `board/phase1_load_ti.sh`, `board/phase1_cycle.sh`. Raw output in [`results/`](results).

**T1.1, load and run: pass.** Loading TI's demo onto remoteproc2 *at runtime* works:
`remote processor 79000000.r5f is now up`, RPMsg channels `rpmsg-tty` and
`rpmsg-client-sample` appear, `/dev/ttyRPMSG0` appears, and the Zephyr shell on the R5
answers `kernel version` with `Zephyr version 3.6.99`. A fresh start reports an R5 uptime
of ~4 s, so it really boots.

We load under a test name (`/lib/firmware/r5test-openamp.elf`) and point remoteproc at it,
instead of TI's `install-openamp.sh`, which also rewrites `/boot/firmware/initrd.img`.

**T1.2, stop and reload without rebooting: fails with TI's demo firmware.**
```
k3_r5_rproc_stop: timeout waiting for rproc completion event
remoteproc remoteproc2: can't stop rproc: -16
```
TI's R5 remoteproc driver asks the firmware to shut down through the mailbox and waits
for an acknowledgement (~10 s timeout) before powering the core off. TI's demo firmware
doesn't answer, so stop fails with `EBUSY` and the core keeps running. Until our firmware
answers that request, **every firmware reload needs a board reboot.** (The exact mailbox
message values still need checking in TI's kernel source.)

**Trap to avoid:** never write `start` to a core that's already running. remoteproc
counts boots: `start` on a running core just adds 1 and returns success, after which
`stop` returns success but doesn't stop anything. Only a reboot clears it.
`phase1_cycle.sh` now refuses to start a core that isn't `offline`.

## Next steps

1. **Zephyr toolchain on the VM**: Zephyr ~3.7 via `west`, the Zephyr SDK's `arm-zephyr-eabi`
   compiler. A few GB.
2. **Rebuild TI's OpenAMP sample ourselves** and check it behaves like the prebuilt one.
3. **Our firmware, step 1**: heartbeat counter in the shared region (`0xA1F00000`), read from Linux.
4. **Shutdown handshake** so stop/reload works without rebooting (retest T1.2).
5. **Phase 2**: the real ring from `capture.c` in shared memory, read by `r5cap_reader.c` on Linux.
6. **Phase 3**: confirm MCU_SPI0 on those pads, remove the spidev overlay, R5 reads the MCP3208.

## Questions for you

1. **Bias point:** `audio/README.md` says a silent mic sits at ~1350 counts, but the preamp
   schematic re-biases to mid-supply (R4 = R7 = 100k), which should read ~2048. Different
   circuit, or a different ADC reference?
2. **Other ADC users:** is anything besides the two mics wired to the MCP3208 (joystick etc.)?
   Once the R5 owns the ADC, Linux can't read other channels directly; the R5 would sample
   them and put them in the ring.
3. **Overlay:** `spidev0` vs `spidev0-fixed`: which is yours, and do you know why the course
   uses `spi-gpio` instead of hardware SPI on these pins?
4. **Backend swap:** OK with adding the R5 as a second backend inside `audio_capture.c`,
   same API, chosen at init, Linux sampler as the default until the R5 path proves better?
5. **Pins:** any objection to the MCU R5F owning the SPI pins? (Linux loses `/dev/spidev0.x`
   while the R5 sampler is in use.)
