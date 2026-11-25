# 8. Glossary and open questions

## Glossary

| Term | Meaning |
|---|---|
| A53 | The four application cores running Linux on the BeagleY-AI |
| Aliasing | Frequencies above fs/2 folding back as fake lower ones after sampling |
| Anti-aliasing filter | Analog low-pass before the ADC to remove content above fs/2 |
| ATCM / BTCM | The R5's small fast internal memories (32 KB each here) |
| Barrier (`dmb`) | Instruction that orders memory accesses as seen by other cores |
| Bearing | Direction (angle) from a node to the sound |
| Bit-banging | Driving a protocol's pins from software instead of a hardware controller |
| Broadside | Straight out from the middle of the mic pair (θ = 0) |
| Chip select (CS) | SPI line that frames a transfer; the MCP3208 needs it high between conversions |
| Confidence | 1 − second peak / main peak; heuristic quality, not a probability |
| Cross-correlation | Sum of products of one signal with a shifted copy of the other, per shift |
| DM timer | TI's general-purpose timer peripheral (MCU_TIMER0 is Zephyr's system timer) |
| DMA | Hardware that moves data without the CPU |
| Far-field | Source far enough that the wavefront is flat across the mics |
| Frame | One sample from each mic |
| fs | Sample rate per mic |
| GCC-PHAT | Correlation in the frequency domain with every frequency weighted equally |
| Interrupt vs counter | A timer can count without interrupting; sleeps need the interrupt |
| Mailbox | Hardware for one core to signal another (interrupts) |
| MCP3208 | 12-bit, 8-channel SPI ADC, one converter behind a multiplexer |
| MPU | Memory Protection Unit: sets access and cache rules per region on the R5 |
| Multiplexer skew | Time between sampling mic 0 and mic 1 (27.6 µs here) |
| Mutation testing | Breaking code on purpose to check the tests notice |
| Near-field | Source close enough that the wavefront is curved; far-field formula biased |
| Nyquist | fs/2, the highest representable frequency |
| OpenAMP | Framework for Linux ↔ remote-core communication (resource table, RPMsg) |
| Peak-to-RMS | Correlation peak divided by the correlation's RMS; low = looks like noise |
| Ping-pong buffer | Two buffers alternated between writer and reader |
| Posted write | A register write applied later by the peripheral; reads may return the old value meanwhile |
| R5F | Arm Cortex-R5F real-time cores on the SoC |
| remoteproc | Linux framework that loads/starts/stops firmware on other cores |
| Resource table | Section in remote firmware describing what it needs from Linux |
| Ring buffer | N slots reused in order; writer never waits |
| RPMsg | Message channels between Linux and a remote core |
| root_squash | NFS mapping root to "nobody", so root can't write to the share |
| Seqlock | Version number checked before and after a copy to detect concurrent writes |
| Spatial aliasing | Wrong-cycle confusion when mic spacing > c/(2f) |
| spidev | Linux's user-space SPI interface (`/dev/spidev0.0`) |
| Springboard | Small boot code at `0x0` that sets up the MPU and jumps to DRAM |
| TDOA | Time difference of arrival between two mics |
| THETA_MIRROR | Flag: the angle could also be its front/back mirror (2 mics) |
| Tickless kernel | Zephyr programs the timer for the next deadline instead of a fixed tick |
| Vector table | Where the CPU starts and finds its exception handlers |
| VIM | The R5's interrupt controller on TI chips |
| Zephyr | The RTOS our R5 firmware is built with |

## Open questions to study

**DSP**
- How accurate is parabolic interpolation vs FFT-upsampled correlation at 5–9 samples of max delay?
- What threshold for peak-to-RMS on real recordings? Same for xcorr and GCC-PHAT?
- Would correlating only the first few ms after onset cut reflection errors?

**Sampling**
- What does a real marble impact's spectrum look like? (Decides spacing and the filter.)
- How much does a skew-correction error of 1 µs cost in degrees at 10/20/30 cm?
- Can the MCP3208 run at 1.25 MHz at 3.3 V without losing accuracy (source impedance, sample-and-hold time)?
- How big is the true per-block gap of the Linux sampler? (Logic analyser on CS.)
- Three mics: per-mic rate drops by 1/3; is full 360° worth it?

**R5**
- Exact mailbox message values for TI's shutdown handshake (TI kernel source).
- Is MCU_SPI0 really on MCU pads 0/2/3/4 on the J722S, and can the R5 drive it?
- Why does the course use `spi-gpio` instead of hardware SPI on these pins?
- How to map R5 timer ticks to Linux CLOCK_REALTIME precisely (shared global counter?).
