// hal_sim.h
// Simulated hardware for unit tests on a PC. Implements r5_hal.h with:
//   - a fake timer that only moves when the test says so
//   - a fake MCP3208 that answers each SPI command with a value the test can predict
// Single-threaded: only the thread running the capture core may call these.
#ifndef HAL_SIM_H
#define HAL_SIM_H

#include <stdint.h>

#define SIM_TICK_HZ 1000000u   // 1 tick = 1 us, so test numbers read as microseconds

// The analog signal on each ADC channel: the 12-bit value the fake chip reports
// for a conversion of this channel that starts at this tick.
typedef uint16_t (*SimSignalFn)(uint8_t channel, uint64_t tick);

void SIM_reset(void);                         // time 0, default signal, no delays, counters cleared
void SIM_setTime(uint64_t tick);
void SIM_advance(uint64_t ticks);
void SIM_setConversionTicks(uint64_t ticks);  // how long each SPI transfer takes (default 0)
void SIM_setSignal(SimSignalFn signal);       // NULL restores the default signal
void SIM_failNextTransfers(uint32_t count);   // the next count transfers return false

uint32_t SIM_getTransferCount(void);          // transfers attempted since reset
uint32_t SIM_getProtocolErrors(void);         // transfers that weren't a valid MCP3208 command

// Default signal: different on every channel, and changes every tick
uint16_t SIM_defaultSignal(uint8_t channel, uint64_t tick);

#endif
