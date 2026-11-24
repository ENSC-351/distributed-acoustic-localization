// test_hal_sim.c
// Checks the fake hardware itself, so later tests that rely on it can be trusted.
#include "hal_sim.h"
#include "mcp3208.h"
#include "r5_hal.h"
#include <stdbool.h>
#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                    \
        }                                                                  \
    } while (0)

// Convert one channel through the HAL, the way the capture core will
static bool convert(uint8_t channel, uint16_t *value)
{
    uint8_t tx[MCP3208_TRANSFER_BYTES];
    uint8_t rx[MCP3208_TRANSFER_BYTES];
    if (!MCP3208_encodeCommand(channel, tx) ||
        !R5HAL_spiTransfer(tx, rx, MCP3208_TRANSFER_BYTES)) {
        return false;
    }
    *value = MCP3208_decodeResult(rx);
    return true;
}

static uint16_t constantSignal(uint8_t channel, uint64_t tick)
{
    (void)channel;
    (void)tick;
    return 0xABC;
}

static void testTimer(void)
{
    SIM_reset();
    CHECK(R5HAL_nowTicks() == 0);
    CHECK(R5HAL_tickHz() == SIM_TICK_HZ);
    SIM_advance(50);
    CHECK(R5HAL_nowTicks() == 50);
    SIM_setTime(1000000);
    CHECK(R5HAL_nowTicks() == 1000000);
}

static void testEveryChannelReadsItsSignal(void)
{
    SIM_reset();
    SIM_setTime(123);
    for (uint8_t ch = 0; ch < MCP3208_NUM_CHANNELS; ch++) {
        uint16_t value = 0;
        CHECK(convert(ch, &value));
        CHECK(value == SIM_defaultSignal(ch, 123));
    }
    CHECK(SIM_getTransferCount() == MCP3208_NUM_CHANNELS);
    CHECK(SIM_getProtocolErrors() == 0);
}

static void testConversionTakesTime(void)
{
    SIM_reset();
    SIM_setConversionTicks(20);
    uint16_t first = 0;
    uint16_t second = 0;
    CHECK(convert(0, &first));
    CHECK(convert(1, &second));
    CHECK(R5HAL_nowTicks() == 40);
    // Each value is the signal at the START of its own transfer
    CHECK(first == SIM_defaultSignal(0, 0));
    CHECK(second == SIM_defaultSignal(1, 20));
}

static void testCustomSignalAndJunkBits(void)
{
    SIM_reset();
    SIM_setSignal(constantSignal);
    uint8_t tx[MCP3208_TRANSFER_BYTES];
    uint8_t rx[MCP3208_TRANSFER_BYTES];
    CHECK(MCP3208_encodeCommand(3, tx));
    CHECK(R5HAL_spiTransfer(tx, rx, MCP3208_TRANSFER_BYTES));
    CHECK(rx[0] == 0xFF);              // junk the decoder must ignore
    CHECK((rx[1] & 0xE0) == 0xE0);
    CHECK((rx[1] & 0x10) == 0);        // null bit
    CHECK(MCP3208_decodeResult(rx) == 0xABC);
    SIM_setSignal(NULL);
    uint16_t value = 0;
    CHECK(convert(3, &value));
    CHECK(value == SIM_defaultSignal(3, 0));
}

static void testInjectedFailures(void)
{
    SIM_reset();
    SIM_setConversionTicks(10);
    SIM_failNextTransfers(2);
    uint16_t value = 0;
    CHECK(!convert(0, &value));
    CHECK(!convert(0, &value));
    CHECK(convert(0, &value));
    CHECK(SIM_getTransferCount() == 3);
    CHECK(R5HAL_nowTicks() == 30);     // failed transfers still take time
}

static void testBadCommandsAreCounted(void)
{
    SIM_reset();
    uint8_t rx[MCP3208_TRANSFER_BYTES] = {0};
    const uint8_t noStartBit[MCP3208_TRANSFER_BYTES] = {0x02, 0x00, 0x00};
    const uint8_t differential[MCP3208_TRANSFER_BYTES] = {0x04, 0x00, 0x00};
    const uint8_t good[MCP3208_TRANSFER_BYTES] = {0x06, 0x00, 0x00};
    R5HAL_spiTransfer(noStartBit, rx, MCP3208_TRANSFER_BYTES);
    R5HAL_spiTransfer(differential, rx, MCP3208_TRANSFER_BYTES);
    R5HAL_spiTransfer(good, rx, 2);    // wrong length
    CHECK(SIM_getProtocolErrors() == 3);
    R5HAL_spiTransfer(good, rx, MCP3208_TRANSFER_BYTES);
    CHECK(SIM_getProtocolErrors() == 3);
}

int main(void)
{
    testTimer();
    testEveryChannelReadsItsSignal();
    testConversionTakesTime();
    testCustomSignalAndJunkBits();
    testInjectedFailures();
    testBadCommandsAreCounted();
    if (failures == 0) {
        printf("test_hal_sim: all checks passed\n");
        return 0;
    }
    printf("test_hal_sim: %d check(s) failed\n", failures);
    return 1;
}
