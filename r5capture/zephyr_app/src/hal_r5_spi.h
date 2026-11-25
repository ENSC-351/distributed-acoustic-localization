// hal_r5_spi.h
// Extra SPI controls for the shell and diagnostics, beyond fw/r5_hal.h.
// Implemented by whichever SPI HAL is built (hal_r5_mcspi.c or hal_r5_gpio.c).
#ifndef HAL_R5_SPI_H
#define HAL_R5_SPI_H

#include <stdbool.h>
#include <stdint.h>

// Short name of the SPI implementation ("mcspi" or "gpio")
const char *R5SPI_name(void);

// Ask for an SPI clock. Returns the clock actually set (from the divider), or 0
// if not supported. Only call while capture is stopped.
uint32_t R5SPI_setClockHz(uint32_t hz);

// One conversion of channel, measuring the time spent shifting bits (from the
// TX write to the RX word being ready) and the whole transfer, in timer ticks.
bool R5SPI_measure(uint8_t channel, uint16_t *value, uint32_t *shiftTicks, uint32_t *totalTicks);

#endif
