# Zephyr firmware for the MCU R5F

Status (2026-11-24): `heartbeat/` passes T1.1 and T1.2 (10/10 restarts without a
reboot) on kernel 7.2.8-arm64-k3-r18. Memory and remoteproc details: `../docs/handoff-joey.md`.

## Build (on the VM)

Zephyr 4.4.2, SDK 1.0.1 (arm-zephyr-eabi), board `beagley_ai/j722s/mcu_r5f0_0`.
Modules: cmsis, cmsis_6, hal_ti, open-amp, libmetal.

```
. /mnt/r5dev/env.sh        # sets ZEPHYR_BASE, the SDK and PATH on Joey's VM
west build -p -b beagley_ai/j722s/mcu_r5f0_0 -d /mnt/r5dev/build-heartbeat r5capture/zephyr/heartbeat
aarch64-linux-gnu-gcc -std=c11 -D_DEFAULT_SOURCE r5capture/zephyr/heartbeat_read.c -o r5_heartbeat_read
```

On the board: `sudo ./heartbeat_cycle.sh zephyr.elf ./r5_heartbeat_read` (script in `../board`).

## Findings

- **Stop/restart works on kernel 7.2.8 with no shutdown handshake.** Madden's 6.1
  TI kernel waits for one; mainline Linux doesn't. Not tested on 6.1.
- **The Zephyr tick timer needs posted mode off.** Otherwise its driver loses the
  compare-enable bit and `k_msleep()` never returns. See `disablePostedMode()`.
- **The tick timer runs at 25 MHz**, not the SoC default of 19.2 MHz (measured:
  6514 ms counted in 5000 ms at 19.2 MHz).
- Code and data must fit in the 32 KB ATCM by default; TI's OpenAMP sample doesn't.
  Heartbeat uses 25 KB, including stacks and Zephyr's 4 KB interrupt table.
- remoteproc needs a `.resource_table` section; an empty one is accepted.
- Memory at `0xA1F00000` is shared with Linux through `/dev/mem`.
