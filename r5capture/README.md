# r5capture: MCP3208 sampling on the R5F core

Moves microphone sampling off Linux and onto the BeagleY-AI's R5F real-time
core. The R5 reads the MCP3208 on a hardware timer and streams samples into
shared memory; Linux reads finished blocks from there. The goal is a faster,
gap-free, evenly spaced stream for direction finding, with **no change** to
the `AudioCapture_*` API that the DSP and `audio_meter` already use.

| | Linux sampler today (`audio/`) | R5 sampler (target) |
|---|---|---|
| Samples per mic | ~9,900/s on Joey's board; **17,200/s measured on ours** | **24,000/s measured, 0 late frames** (ADC limit ~31,000/s) |
| Time per conversion | ~50 us (19 clocks + ~23 us overhead) | ~20 us (19 clocks at 1 MHz + < 1 us) |
| Stream | 128-frame blocks with gaps between them | continuous, no gaps |
| Timing | measured per block; some blocks "disturbed" | hardware timer; every frame on time |
| Depends on CPU clock | yes (bit-banged SPI) | no |

It does **not** change the MCP3208 itself: channels are still converted one
after another (fixed, known offset between mics), and the 19 clocks per
conversion set the ceiling.

## Documents

- [docs/design.md](docs/design.md): architecture, timing budget, shared-memory protocol, timestamps
- [docs/plan.md](docs/plan.md): phases, tasks, exit criteria, open questions, risks
- [docs/test-plan.md](docs/test-plan.md): how each phase is tested and what "passing" means
- [docs/study/](docs/study/README.md): study guide covering the theory and everything learned so far

## Layout

```
r5capture/
  include/r5cap_shared.h   shared-memory contract (R5 and Linux both compile it)    [done, tested]
  fw/                      portable firmware: MCP3208 encoding, capture core, HAL    [done, tested]
                           R5 hardware layer (hal_r5.c, main.c, Zephyr app)          [next]
  linux/                   Linux reader for the ring; later the AudioCapture backend [done, tested]
  test/                    PC unit tests with simulated hardware; make test / board  [done]
  board/                   scripts run on the BeagleY-AI
  docs/                    design, plan, test plan, handoff, raw board results
```

## Status (2026-11-24)

**Latest status and next steps: [CONTEXT.md](CONTEXT.md).** Phase 1 now passes on kernel 7.2.8
(see [zephyr/README.md](zephyr/README.md)). The list below is the earlier status.

- **Phase 0 done:** shared-memory contract, MCP3208 encoding, capture core and Linux reader,
  all unit-tested on the VM and on the board (`test/`, `make test`, `make board`).
- **Phase 1 half done:** the MCU R5F (remoteproc2) runs TI's prebuilt Zephyr firmware loaded
  at runtime, and talks to Linux over RPMsg (T1.1 pass). Stop/reload without a reboot fails
  with that firmware (T1.2): it doesn't answer TI's shutdown request.
- Full write-up of results and board findings: [docs/handoff-joey.md](docs/handoff-joey.md).
- Joey's Linux sampler (`audio/`) stays the working path until the R5 path passes every test
  in [docs/test-plan.md](docs/test-plan.md).
