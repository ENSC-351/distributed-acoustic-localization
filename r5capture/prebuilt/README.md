# Prebuilt binaries (for testing without the toolchains)

Built from this commit on Madden's VM, 2026-11-25. Rebuild instead whenever the
sources change; these are only here so the R5 sampler can be tested on another
board without installing Zephyr 4.5.0.

| File | Runs on | Built from |
|---|---|---|
| `r5capture-mcspi.elf` | MCU R5F (remoteproc `79000000.r5f`) | `../zephyr_app`, Zephyr 4.5.0, `-DR5CAP_SPI=mcspi` |
| `r5cap_ctl` | BeagleY-AI Linux (aarch64) | `../linux/r5cap_ctl.c`: start/stop/status of R5 sampling via shared memory |
| `r5cap_status` | aarch64 | `../linux/r5cap_status.c`: ring counters |
| `r5cap_ring_read` | aarch64 | `../linux/r5cap_ring_read.c` + `r5cap_reader.c`: R5 ring → WAV + timing text |
| `r5cap_hb_read` | aarch64 | `../linux/r5cap_hb_read.c`: R5 heartbeat check |
| `adc_record` | aarch64 | `../linux/adc_record.c` + `../../hal/src/adc.c`: Linux recording (same output format) |

Rebuild:
```
source ~/zephyrproject/.venv/bin/activate
west build -p always -b beagley_ai/j722s/mcu_r5f0_0 r5capture/zephyr_app -d build/r5app-mcspi -- -DR5CAP_SPI=mcspi
# Linux tools: see the aarch64-linux-gnu-gcc lines in ../docs/test-r5-adc-joey.md
```
