# Study guide: distributed acoustic localization

Everything learned on this project up to 2026-11-25, organized to study from.
Read in order; each file builds on the previous ones. Numbers are **measured** on
our BeagleY-AI unless marked *estimate*. Raw data: [`../results/`](../results).

| # | File | What you'll understand after |
|---|---|---|
| 1 | [01-system-overview.md](01-system-overview.md) | What the whole system does, who built what, how data flows from microphone to master |
| 2 | [02-tdoa-direction-finding.md](02-tdoa-direction-finding.md) | How two microphones give a direction: delay, angle formula, cross-correlation, GCC-PHAT, refusals, confidence |
| 3 | [03-sampling-and-the-adc.md](03-sampling-and-the-adc.md) | Why sample rate and mic spacing matter, the MCP3208's limits, aliasing, multiplexer skew |
| 4 | [04-linux-sampler-measured.md](04-linux-sampler-measured.md) | How Joey's Linux sampler works and what it actually achieves on our board |
| 5 | [05-shared-memory-ring.md](05-shared-memory-ring.md) | Passing data between cores: ring vs ping-pong, seqlocks, barriers, caches, and how we tested it |
| 6 | [06-r5-bringup.md](06-r5-bringup.md) | The R5 core: remoteproc, RPMsg, Zephyr, memory map, and the bugs we hit and how we found them |
| 7 | [07-testing-and-debugging-methods.md](07-testing-and-debugging-methods.md) | The general techniques that worked: simulation, mutation tests, stress tests, register dumps |
| 8 | [08-glossary-and-questions.md](08-glossary-and-questions.md) | Every term in one place, plus open questions to study |

Quick-reference numbers:

| Quantity | Value |
|---|---|
| Speed of sound | 343 m/s (≈ 34 cm per ms, ≈ 1 cm per 29 µs) |
| Max delay at 10 cm spacing | 0.29 ms |
| Linux sampler on our board | 17,200 samples/s per mic, 27.6 µs mic-to-mic skew |
| MCP3208 conversion | 19 SPI clocks; ceiling ~25,000/s per mic at 1 MHz (*estimate*) |
| DSP estimate on the board | ~7 ms per 960-sample window (full cross-correlation) |
| R5 heartbeat | 20 updates in 2.004 s (100 ms period), read correctly from Linux |
