# R5 sampling: where we stopped (2026-11-24)

Start here. Branch `jkolsteren/feature/r5-capture` = Joey's Linux reserved-core
sampler (`jkolsteren/feature/audio-meter`) + Madden's `madden/r5-capture` + the
Zephyr work below. Earlier background: `docs/handoff-joey.md` (its Phase 1 status
and "next steps" are older than this file). Findings and build notes: `zephyr/README.md`.

## Status

| Phase | State |
|---|---|
| 0 Portable code | Done (Madden). Tests pass on Joey's VM too: `cd test && make test` |
| 1 R5 bring-up | **Done on Joey's board** (kernel 7.2.8): our Zephyr firmware loads, T1.2 10/10 restarts without a reboot |
| 2 Shared memory | **Next.** The heartbeat shows R5 -> Linux memory works at `0xA1F00000`; the ring hasn't run on the R5 |
| 3 SPI, 4 timer, 5 signal, 6 integration | Not started |

On Madden's board (TI kernel 6.1.83) stop **does** fail: see "Madden's board" below.

## Decisions

- Keep the Linux sampler as the fallback; add the R5 as a second backend in
  `audio_capture.c`, chosen at init, same API. Note that the Linux version now
  requires the overlay and an `isolcpus` core (`../overlays/README.md`).
- Nothing but the two mics is on the ADC.
- Madden has handed the R5 work to Joey for now.

## Next steps

1. **Phase 2:** new Zephyr app next to `zephyr/heartbeat` (copy its CMakeLists,
   `rsc_table.ld`, `prj.conf` and `disablePostedMode()`) that runs `fw/capture.c`
   with a counting pattern, ring at `0xA1F00000`. On Linux, map it with
   `/dev/mem` + `O_SYNC` and read it with `linux/r5cap_reader.c`. Then T2.1 to T2.3.
   - Check that the ring is uncached on the R5. Zephyr's MPU setup only defines
     the SRAM region, so this address uses the default map, which should be
     uncached (the heartbeat worked, but didn't test this under load).
   - ATCM is 32 KB and the heartbeat uses 25 KB. If it doesn't fit, trim Zephyr
     (for example `CONFIG_NUM_IRQS`, stacks) before moving code to DDR.
2. **Phase 3:** MCU_SPI0 on the ADC's pins is confirmed working from Linux; pad
   values are in `../audio/CONTEXT.md`. Linux must release it first: add a boot
   entry without the `mcu-spi0-adc` overlay, and keep `hwspi`. Not checked: whether
   MCU_SPI0 then stays powered and clocked for the R5. Zephyr has a McSPI driver
   (`spi_omap_mcspi.c`); not tried.
   - **Change the ADC transfer:** `fw/mcp3208.c` sends 24 clocks, 48 us per two-mic
     frame at 1 MHz, which breaks T4.4's 90% limit at 20 kHz. Use one 19-clock word
     (38 us), as `MODE_WORD19` in `../hal/src/adc.c` does.
3. **Phase 4:** `mcu_timer1` to `mcu_timer3` are marked reserved in Joey's device
   tree, so Linux leaves them alone. Zephyr's tick uses timer 0. Watch for the same
   posted-mode problem on any of them.

## Set up the toolchain on another machine

On Joey's VM it is in `/mnt/r5dev` (`. /mnt/r5dev/env.sh`). Elsewhere:

```
python3 -m venv ~/r5dev/venv && . ~/r5dev/venv/bin/activate && pip install west ninja
git clone --depth 1 -b v4.4.2 https://github.com/zephyrproject-rtos/zephyr ~/r5dev/zephyrproject/zephyr
cd ~/r5dev/zephyrproject && west init -l zephyr
west config manifest.project-filter -- '-.*,+cmsis,+cmsis_6,+hal_ti,+open-amp,+libmetal'
west update --narrow -o=--depth=1
pip install -r zephyr/scripts/requirements-base.txt
```

Then get Zephyr SDK 1.0.1 (minimal bundle, with the `arm-zephyr-eabi` GNU toolchain
unpacked in its `gnu/` folder), install `cmake` and `device-tree-compiler`, and set
`ZEPHYR_BASE` and `ZEPHYR_SDK_INSTALL_DIR`. About 2 GB in total.

## Board notes

- The MCU R5F is remoteproc `79000000.r5f`. Don't write `start` to a running core.
- Files loaded for tests are named `/lib/firmware/r5test-*`; nothing loads at boot.
- TI's prebuilt Zephyr firmware (`/opt/ti-zephyr-firmware`) isn't on Joey's board,
  so `board/phase1_load_ti.sh` only works on Madden's.
- Joey's `hwspi` boot entry also sets up the Linux sampler. The heartbeat test
  needs no boot changes.

## Also open

- The pushed Linux sampler (after the shared-core code was removed) hasn't been
  rerun on the board: `sudo ./audio_meter --bench`.
- Two separate mics are untested; the measurements used one mic on both inputs.

## Madden's board (TI kernel 6.1.83, Debian 12), tested 2026-11-25

- **T1.1 passes, T1.2 fails.** Loading and running works (TI's prebuilt firmware, and
  our own builds). Stop fails every time: `k3_r5_rproc_stop: timeout waiting for rproc
  completion event`, `can't stop rproc: -16`. The TI 6.1 driver waits ~10 s for the
  firmware to acknowledge a shutdown request over the mailbox; neither firmware does.
  On this kernel every reload needs a reboot until the firmware answers that request.
- **Second firmware variant: `zephyr_app/`** (Zephyr **4.5.0**). Runs from DRAM at
  0xA1200000 (4.5.0 adds a boot springboard at 0x0), so the 32 KB ATCM limit doesn't
  apply; includes OpenAMP/RPMsg and the Zephyr shell over `/dev/ttyRPMSG0` (shell
  commands `r5cap` and `r5timer` dump the heartbeat and the timer registers). Heartbeat
  PASS: 20 updates in 2.004 s.
- **Same timer bug, different fix.** `zephyr_app/src/timer_fix.c` waits for TWPS and
  sets compare-enable after the driver's init; `zephyr/heartbeat` turns posted mode off.
  Both work. Measured: `TCLR=0x3` before, `0x43` after, `TSICR=0x4` (posted mode).
- **Don't enable `CONFIG_CACHE_MANAGEMENT`.** It turns the R5 data cache on; only DRAM
  has an MPU region, device registers become cacheable, and the firmware stalls (no RPMsg,
  sleeps hang). With the cache off, the shared region needs no flushes.
- **The two heartbeats differ** (same address, different magic, fields and period):
  `zephyr/heartbeat_read.c` reads `zephyr/heartbeat`, `linux/r5cap_hb_read.c` reads
  `zephyr_app`. Each rejects the other's magic. Unify before Phase 2.
- **Bit-banged Linux sampler on kernel 6.1: 17,177 samples/s per mic** long-run
  (16-frame blocks, `linux/sampler_stats.c`, results in `docs/results/stats_*.txt`),
  vs 9,827 on Joey's 7.2.8 board. Kernel versions clearly differ a lot here.
- Theory and all findings so far, as a study guide: `docs/study/`.

## R5 sampling the ADC (Madden's board, 2026-11-25, later)

Start with `docs/test-r5-adc-joey.md` (test steps) and `docs/study/` (background).
- **Working:** `zephyr_app` (Zephyr 4.5.0) samples the MCP3208 through MCU_SPI0 (Joey's overlay;
  Linux keeps it powered with `board/mcspi_power_on.sh`), one 19-bit word per conversion, timer-paced
  frames, ring at 0xA1F00000, control through shared memory (`include/r5cap_control.h`, `linux/r5cap_ctl`).
- **Best clean: 16,000 frames/s per mic at ~1.23 MHz with interrupts off** (`r5cap_ctl start 16000 1250000 irqoff`).
  A 50 Hz interrupt (likely the Zephyr kernel timer) makes frames late when interrupts are on.
- **Ceiling ~18,900 frames/s** (R5 code speed; DRAM data is uncached). Next optimisations: two
  conversions per McSPI FIFO burst, and capture data in BTCM. SPI alone allows ~21,800.
- **Measured on the R5:** McSPI reference clock 48 MHz; bit-banging from the R5 is ~100 us per
  conversion (each register access ~0.7 us), so `R5CAP_SPI=gpio` is only a fallback.
- **I-cache on, D-cache off** (`zephyr_app/src/icache.c`): enabling the D-cache stalls the firmware.
- Prebuilt firmware + tools in `prebuilt/` (built from the commit that added them).
- Open: tap recording through the R5 not analyzed yet; CH0 ~1,890 vs CH1 ~2,360 on Madden's
  board with the same signal supposedly on both; the identity of the 50 Hz interrupt; the
  `Deferred event dump` dmesg lines; one unexplained full-board freeze during an R5 load.

### Update (2026-11-25, evening): 20,000 frames/s clean
- Per-frame variables moved to the ATCM (`.r5fast` at 0x1000, `zephyr_app/r5fast.ld`, default on):
  **clean rate 16,000 -> 20,000 frames/s per mic** (0 late, worst frame start 13.6 us). Ceiling ~20,500.
- BTCM at 0x41010000 is NOT accessible from the MCU R5F (synchronous external abort); found with the
  new crash recorder (`zephyr_app/src/fatal.c`, `r5cap_ctl crash`).
- Profile (`r5cap_ctl start <rate> <spi> irqoff profile`, then `diag`): at 16k, bit shifting ~34 us,
  SPI register work ~7.8 us, bookkeeping ~15.7 us per frame (probe-inflated). Next: capture thread
  stack into ATCM (stack is still uncached DRAM), then two conversions per McSPI FIFO burst.
