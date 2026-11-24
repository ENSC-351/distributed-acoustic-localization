// hal_sim.c
// Fake hardware for tests: implements R5HAL_* (r5_hal.h) plus SIM_* controls (hal_sim.h).
#include "hal_sim.h"
#include "mcp3208.h"
#include "r5_hal.h"
#include <stdbool.h>
#include <stddef.h>

// Command bits the fake chip checks (see the table in mcp3208.c)
#define CMD_PADDING_MASK 0xF8        // top 5 bits of byte 0 must be 0
#define CMD_START_BIT 0x04
#define CMD_SINGLE_ENDED_BIT 0x02
#define CMD_D2_BIT 0x01              // channel bit 2 is bit 0 of byte 0
#define CMD_D2_SHIFT 2
#define CMD_D1D0_SHIFT 6             // channel bits 1-0 are the top 2 bits of byte 1
#define CMD_D1D0_MASK 0x03

// The real chip leaves these reply bits undefined; the fake fills them with
// junk so the decoder must ignore them
#define JUNK_BYTE0 0xFF
#define JUNK_BYTE1_TOP 0xE0          // bits 7-5 of byte 1 (bit 4 is the null bit: always 0)
#define VALUE_HIGH_SHIFT 8
#define VALUE_HIGH_MASK 0x0F
#define VALUE_LOW_MASK 0xFF

#define DEFAULT_SIGNAL_SLOPE 3u
#define DEFAULT_SIGNAL_CHANNEL_STEP 1000u

static uint64_t nowTicks = 0;
static uint64_t conversionTicks = 0;
static SimSignalFn signalFn = SIM_defaultSignal;
static uint32_t failRemaining = 0;
static uint32_t transferCount = 0;
static uint32_t protocolErrors = 0;

uint16_t SIM_defaultSignal(uint8_t channel, uint64_t tick)
{
    return (uint16_t)((tick * DEFAULT_SIGNAL_SLOPE + channel * DEFAULT_SIGNAL_CHANNEL_STEP) &
                      MCP3208_MAX_READING);
}

void SIM_reset(void)
{
    nowTicks = 0;
    conversionTicks = 0;
    signalFn = SIM_defaultSignal;
    failRemaining = 0;
    transferCount = 0;
    protocolErrors = 0;
}

void SIM_setTime(uint64_t tick)
{
    nowTicks = tick;
}

void SIM_advance(uint64_t ticks)
{
    nowTicks += ticks;
}

void SIM_setConversionTicks(uint64_t ticks)
{
    conversionTicks = ticks;
}

void SIM_setSignal(SimSignalFn signal)
{
    signalFn = (signal != NULL) ? signal : SIM_defaultSignal;
}

void SIM_failNextTransfers(uint32_t count)
{
    failRemaining = count;
}

uint32_t SIM_getTransferCount(void)
{
    return transferCount;
}

uint32_t SIM_getProtocolErrors(void)
{
    return protocolErrors;
}

static bool isValidCommand(const uint8_t *tx, uint32_t len)
{
    return tx != NULL && len == MCP3208_TRANSFER_BYTES &&
           (tx[0] & CMD_PADDING_MASK) == 0 &&
           (tx[0] & CMD_START_BIT) != 0 &&
           (tx[0] & CMD_SINGLE_ENDED_BIT) != 0;
}

bool R5HAL_spiTransfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    transferCount++;
    uint64_t startTick = nowTicks;   // the chip samples at the start of the transfer
    nowTicks += conversionTicks;     // a transfer takes time, even if it fails

    if (failRemaining > 0) {
        failRemaining--;
        return false;
    }
    if (rx == NULL) {
        protocolErrors++;
        return true;
    }
    if (!isValidCommand(tx, len)) {
        // A real chip doesn't report a bad command: it just answers garbage
        protocolErrors++;
        for (uint32_t i = 0; i < len; i++) {
            rx[i] = JUNK_BYTE0;
        }
        return true;
    }

    uint8_t channel = (uint8_t)(((tx[0] & CMD_D2_BIT) << CMD_D2_SHIFT) |
                                ((tx[1] >> CMD_D1D0_SHIFT) & CMD_D1D0_MASK));
    uint16_t value = (uint16_t)(signalFn(channel, startTick) & MCP3208_MAX_READING);
    rx[0] = JUNK_BYTE0;
    rx[1] = (uint8_t)(JUNK_BYTE1_TOP | ((value >> VALUE_HIGH_SHIFT) & VALUE_HIGH_MASK));
    rx[2] = (uint8_t)(value & VALUE_LOW_MASK);
    return true;
}

uint64_t R5HAL_nowTicks(void)
{
    return nowTicks;
}

uint32_t R5HAL_tickHz(void)
{
    return SIM_TICK_HZ;
}
