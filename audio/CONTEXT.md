# Context: Linux reserved-core sampler

**Which version this is:** sampling done by Linux through hardware SPI, on a CPU
core reserved for it. It comes after the bit-banged (`spi-gpio`) Linux version
(commit `d341a96`) and before the planned R5 version (`madden/r5-capture`).
Status as of 2026-11-24: working on one BeagleY-AI, kernel 7.2.8, Debian 13.

## How it works

- `overlays/` puts header pins 19/21/23/24 on the MCU_SPI0 controller.
- Boot arguments: `isolcpus=3 rcu_nocbs=3 irqaffinity=0-2 spidev.bufsiz=65536`.
- `audio_capture.c` pins its sampling thread to the reserved core, at normal
  priority (real-time priority starves kernel work there and stalls the system).
- Each block is one SPI message. `AudioCapture_init()` fails if no core is reserved.

## Measurements

One mic wired to both ADC channels, 128-frame blocks, 3 s per run:

| Version | SPI | Rate in blocks | Overall | Max gap |
|---|---|---|---|---|
| Bit-banged (16-frame blocks) | ~0.7 MHz | 9,827 Hz | 9,251 Hz | 606 us |
| Hardware SPI, shared core | 1 MHz | 12,797 Hz | 12,241 Hz | 614 us |
| **Hardware SPI, reserved core** | **1 MHz** | **12,792 Hz** | **12,772 Hz** | **33 us** |
| Hardware SPI, reserved core | 1.25 MHz | 14,205 Hz | 14,176 Hz | 23 us |

Two-minute run at 1 MHz: 11,889 blocks, 6 disturbed, max gap 154 us, 0 ADC
errors, no kernel stall messages. These were measured just before the
shared-core fallback was removed; the sampling loop is otherwise the same.

## Limits

- Not strictly gapless. Gaps between blocks are small and reported in `gapBeforeNs`.
- Uses one of the four A53 cores completely.
- At 1 MHz about 18 us of each 39 us conversion is fixed Linux cost, which is
  what the R5 version should remove.
- The ADC mean falls slightly as the SPI clock rises (1341 at 1 MHz, 1334 at
  2 MHz). 2 MHz is out of spec at 3.3 V.

## Useful for the R5 version

- **MCU_SPI0 works on the ADC's pins on the J722S/AM67A**, transmit and receive.
  Pads of `mcu_pmx0`, mux mode 0: 0x00 CS0, 0x08 CLK, 0x0c D0 = MOSI, 0x10 D1 = MISO.
  In the working overlay CLK and MISO have input enabled (0x00050000), and D0 is
  output, D1 input (`ti,pindir-d0-out-d1-in`).
- The MCP3208 accepts one 19-clock word per conversion (start bit first); see
  `MODE_WORD19` in `hal/src/adc.c`. The 3-byte form takes 24 clocks, which at
  1 MHz is 48 us for two channels: almost all of a 50 us frame.
- Both versions drive MCU_SPI0, so don't run them together. Boot without this
  overlay when the R5 owns the ADC.
- A silent mic reads about 1341 counts at 1 MHz on this hardware.
- The public API (`audio_capture.h`) is meant to stay the same for the R5 version.

## Not done

- Two separate mics have not been tested.
- The overlay has been tested on one board and kernel only.
