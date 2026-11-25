# zephyr_app: Zephyr 4.5.0 firmware for the MCU R5F (OpenAMP variant)

Second firmware variant next to `../zephyr/heartbeat` (Joey's, Zephyr 4.4.2, ATCM only).
This one is Zephyr's OpenAMP resource-table sample (`src/main_remote.c`, unchanged) plus
a heartbeat (`src/heartbeat.c`) and a timer-driver workaround (`src/timer_fix.c`).

- Runs from DRAM (0xA1200000, 13 MB; the overlay keeps 0xA1F00000+ for the shared region).
- RPMsg + Zephyr shell over `/dev/ttyRPMSG0`: `kernel uptime`, `kernel thread list`,
  `r5cap` (heartbeat), `r5timer` (system timer registers).
- Tested on Madden's board (TI kernel 6.1.83): loads and runs; stop needs the shutdown
  handshake there (see `../CONTEXT.md`).

## Build (Zephyr 4.5.0 needs CMake >= 3.28 and Python >= 3.12)

```
source ~/zephyrproject/.venv/bin/activate          # Madden's VM
west build -p always -b beagley_ai/j722s/mcu_r5f0_0 r5capture/zephyr_app -d build/r5app
# output: build/r5app/zephyr/r5capture.elf
```

## Run (on the board, right after a reboot on kernel 6.1)

```
sudo ./phase1_diag.sh r5capture-hb.elf | tee diag.txt      # scripts in ../board
```
