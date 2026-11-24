// test_mcp3208.c
// T0.1: MCP3208 command encoding and result decoding. Runs on the PC.
#include "mcp3208.h"
#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static void testEncodeAllChannels(void)
{
    // Expected command bytes for every channel (same encoding as hal/src/adc.c)
    const uint8_t expected[MCP3208_NUM_CHANNELS][2] = {
        {0x06, 0x00}, {0x06, 0x40}, {0x06, 0x80}, {0x06, 0xC0},
        {0x07, 0x00}, {0x07, 0x40}, {0x07, 0x80}, {0x07, 0xC0},
    };
    for (uint8_t ch = 0; ch < MCP3208_NUM_CHANNELS; ch++) {
        // Start with a pattern so we know the function wrote every byte
        uint8_t tx[MCP3208_TRANSFER_BYTES] = {0xAA, 0xAA, 0xAA};
        CHECK(MCP3208_encodeCommand(ch, tx));
        CHECK(tx[0] == expected[ch][0]);
        CHECK(tx[1] == expected[ch][1]);
        CHECK(tx[2] == 0x00);
    }
}

static void testEncodeRejectsBadChannel(void)
{
    uint8_t tx[MCP3208_TRANSFER_BYTES] = {0xAA, 0xAA, 0xAA};
    CHECK(!MCP3208_encodeCommand(MCP3208_NUM_CHANNELS, tx));
    CHECK(!MCP3208_encodeCommand(255, tx));
    // A rejected call must leave the buffer untouched
    CHECK(tx[0] == 0xAA && tx[1] == 0xAA && tx[2] == 0xAA);
}

static void testDecode(void)
{
    CHECK(MCP3208_decodeResult((const uint8_t[]){0x00, 0x00, 0x00}) == 0);
    CHECK(MCP3208_decodeResult((const uint8_t[]){0x00, 0x0F, 0xFF}) == MCP3208_MAX_READING);
    CHECK(MCP3208_decodeResult((const uint8_t[]){0x00, 0x08, 0x00}) == 2048);
    // Bits outside the result (all of byte 0, top nibble of byte 1) are undefined
    // on the real chip and must be ignored
    CHECK(MCP3208_decodeResult((const uint8_t[]){0xFF, 0xFA, 0xBC}) == 0xABC);
    CHECK(MCP3208_decodeResult((const uint8_t[]){0xFF, 0xF0, 0x00}) == 0);
}

static void testEveryValueRoundTrips(void)
{
    // Fake chip reply for every possible 12-bit value, with junk in the
    // undefined bits: decode must return exactly the value
    for (uint16_t value = 0; value <= MCP3208_MAX_READING; value++) {
        const uint8_t rx[MCP3208_TRANSFER_BYTES] = {
            0xFF,
            (uint8_t)(0xE0 | (value >> 8)),   // junk in bits 7-5, null bit 0, B11-B8
            (uint8_t)(value & 0xFF),
        };
        if (MCP3208_decodeResult(rx) != value) {
            CHECK(MCP3208_decodeResult(rx) == value);
            break;   // one failure report is enough
        }
    }
}

int main(void)
{
    testEncodeAllChannels();
    testEncodeRejectsBadChannel();
    testDecode();
    testEveryValueRoundTrips();
    if (failures == 0) {
        printf("test_mcp3208: all checks passed\n");
        return 0;
    }
    printf("test_mcp3208: %d check(s) failed\n", failures);
    return 1;
}
