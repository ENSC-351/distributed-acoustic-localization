// r5_hal.h
// Hardware abstraction layer: the only things the capture core needs from the
// hardware. The core (capture.c) calls these and nothing else hardware-specific,
// so it builds and runs unchanged on the R5 and in tests on a PC.
//
// Implementations:
//   hal_r5.c   on the R5F: McSPI (or GPIO) and a hardware timer   (written once
//              we know the course toolchain and which pins the ADC is on)
//   hal_sim.c  for unit tests on a PC: a fake timer and a fake MCP3208
//
// Not in here on purpose: the timer interrupt. The core doesn't start the
// timer; the platform's main/ISR calls capture_tick() once per frame. That way
// tests can call capture_tick() directly and control time exactly.
#ifndef R5_HAL_H
#define R5_HAL_H

#include <stdbool.h>
#include <stdint.h>

// One SPI transfer, SPI mode 0. Chip select goes low before the first bit and
// high after the last, so every call is one complete MCP3208 conversion.
// Sends len bytes from tx while receiving len bytes into rx. Blocks until done.
// Returns false if the transfer failed (rx contents are then undefined).
bool R5HAL_spiTransfer(const uint8_t *tx, uint8_t *rx, uint32_t len);

// Free-running timer, in ticks of R5HAL_tickHz(). Never goes backwards.
// 64 bits so it never wraps in practice; on hardware with a 32-bit timer the
// implementation extends it by counting wraps.
uint64_t R5HAL_nowTicks(void);

// Frequency of R5HAL_nowTicks(), in Hz. Constant while running.
uint32_t R5HAL_tickHz(void);

#endif
