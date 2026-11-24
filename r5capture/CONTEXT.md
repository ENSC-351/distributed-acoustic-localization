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

Not tested: anything on Madden's 6.1 kernel (its driver waits for a shutdown
handshake that this firmware doesn't have, so stop probably still fails there).

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
