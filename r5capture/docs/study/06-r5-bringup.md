# 6. Bringing up the R5 core

## The cores on the BeagleY-AI (TI AM67A / J722S)

Four Cortex-A53 cores run Linux. Extra cores, seen by Linux as `remoteproc` devices:

| remoteproc | Core | State at boot | Usable |
|---|---|---|---|
| 0, 1 | C7x DSPs | offline | (not ours) |
| 2 | `79000000.r5f`, **MCU-domain R5F** | offline | **yes, our target** |
| 3 | `78000000.r5f` | attached, runs TI's device-manager firmware | no |
| 4 | `78400000.r5f`, MAIN-domain R5F | offline | yes |

We chose the MCU R5F because the ADC's SPI pins are MCU-domain pads (0, 2, 3, 4 of the
MCU pad controller), likely the native pins of the hardware **MCU_SPI0** (true on the
related AM62x; to confirm for J722S).

## remoteproc: how Linux runs code on the R5

```
cp firmware.elf /lib/firmware/NAME
echo NAME  > /sys/class/remoteproc/remoteproc2/firmware
echo start > /sys/class/remoteproc/remoteproc2/state      # → "running"
```
- The ELF needs a **resource table** (`.resource_table` section): it tells Linux what the
  firmware needs (memory, virtio devices for RPMsg, trace buffers).
- **RPMsg / OpenAMP**: message channels between Linux and the R5 over shared memory +
  mailbox interrupts. They show up on Linux as `/dev/rpmsg*` and `/dev/ttyRPMSG*`.
  The Zephyr sample even runs a **shell on the R5** reachable through `/dev/ttyRPMSG0`.
- We load under a **test name** at runtime instead of TI's install script, which also
  rewrites `/boot/firmware/initrd.img`.

**Stopping needs the firmware's cooperation.** TI's driver sends a shutdown request through
the mailbox and waits (~10 s) for an acknowledgement before powering the core off. TI's demo
and our firmware don't answer yet → `stop` fails with EBUSY and the core keeps running.
Until we add the handshake: **reboot before every firmware load.**

**The boot-count trap.** remoteproc counts starts. `start` on a running core just adds 1 and
says "ok"; after that `stop` also says "ok" but doesn't stop anything. Never start a core
that isn't `offline`.

## Memory map of the MCU R5F

| Address | Size | Use |
|---|---|---|
| `0x00000000` | 32 KB | ATCM (fast internal RAM): vector table / boot springboard |
| `0xA1000000` | 1 MB | DMA region: RPMsg vrings (Linux `mcu-r5fss-dma-memory-region`) |
| `0xA1100000` | 1 MB | Resource table |
| `0xA1200000` | 13 MB | Firmware code and data (DRAM) |
| `0xA1F00000` | 1 MB | **r5capture shared region** (heartbeat now, ring later) |

The board's device tree gives Zephyr 14 MB at `0xA1200000`; our overlay shrinks it to 13 MB
so nothing Zephyr links or allocates can land in our region. Linux reads it through
`/dev/mem` (allowed because the carve-out is `no-map`, not normal RAM).

## Zephyr: which version and why

| Version | What happened |
|---|---|
| 3.7.0 | No BeagleY-AI board, no J722S support |
| 4.1–4.4 | Board exists, but everything is placed in the 32 KB ATCM → OpenAMP sample overflows by 44 KB |
| 4.3.2 + overlay moving RAM to DRAM | Builds, but no code at `0x0` → the R5 would start in empty memory |
| **4.5.0** | Two upstream fixes: DRAM as main memory **and** a boot "springboard" at `0x0` that sets up the MPU and jumps to DRAM. Matches TI's working firmware layout |

4.5.0 needs CMake ≥ 3.28 and Python ≥ 3.12 (Debian 12 has 3.25 and 3.11): solved with a
pip-installed CMake and a uv-installed Python in the venv.

**Verify before trusting a build:** we compared our ELF's segments with TI's working one
(entry point `0x0`, segment at `0x0`, byte-identical resource table) before loading it.

## Bug 1: turning the cache on stalled everything

Symptom: magic written, then counter stuck, and no RPMsg channels. Cause: I enabled
`CONFIG_CACHE_MANAGEMENT` for a cache flush; on this SoC that makes the boot code **turn the
R5's data cache on**. The springboard only creates an MPU region for DRAM, so device
registers (timer, mailbox) fall back to the default map and can be read from stale cache.
Fix: keep the cache off (as TI's firmware does). Lesson: change one thing at a time from a
known-good build, and compare configs (`.config` diff showed it immediately).

## Bug 2: the timer counted but never interrupted (Zephyr driver bug)

Symptom: uptime increased, but a 100 ms sleep never ended; thread list showed the heartbeat
thread "sleeping" for 20+ s. Key insight: Zephyr's **uptime reads the counter directly**,
while **sleeps need the timer interrupt**. So: counter OK, interrupt missing.

Register dump of the system timer (MCU_TIMER0) read on the R5:
```
TCLR=0x3   start + auto-reload, but CE (compare enable, bit 6) missing
IRQSTATUS_RAW=0   never matched → never interrupted
TSICR=0x4  posted-write mode on
```
Cause: the driver sets `TCLR` bits one at a time with read-modify-write. In **posted mode**
the timer applies writes later in its own clock domain, and a read in the meantime returns the
old value. The "set ST" step read TCLR before "set CE" had landed and wrote it back without CE.
Fix (`zephyr_app/src/timer_fix.c`): right after the driver's init, wait until `TWPS` shows no
TCLR write pending, then set CE. After: `TCLR=0x43`, heartbeat 20 updates in 2.004 s. Still
unfixed upstream (2026-11-25); worth reporting.

Why TI's demo didn't show it: nothing in it ever sleeps (shell and RPMsg run on mailbox interrupts).

## Where Phase 1 stands

Done: our firmware (Zephyr 4.5.0) loads, runs, keeps time, shares memory with Linux, and
talks RPMsg. Next: shutdown handshake (reload without reboot), then the real ring (Phase 2),
then SPI to the MCP3208 (Phase 3).
