/**
 * @file mcp3208.c
 * @author Madden Wynans (you@domain.com)
 * @brief One conversion is a 3 byte SPI transfer.
 * @version 0.1
 * @date 2026-11-24
 * 
 * @copyright Copyright (c) 2026
 * @example
 * tx byte 0:  0  0  0  0  0  START SGL D2     START=1, SGL=1 (single-ended), D2 = channel bit 2
 * tx byte 1:  D1 D0 x  x  x  x     x   x      channel bits 1 and 0
 * tx byte 2:  x  x  x  x  x  x     x   x      don't care 
 * 
 * rx byte 0:  ?  ?  ?  ?  ?  ?     ?   ?      nothing yet (chip is still being told what to do)
 * rx byte 1:  ?  ?  ?  0  B11 B10  B9  B8     0 = null bit, then the top 4 result bits
 * rx byte 2:  B7 B6 B5 B4 B3  B2   B1  B0     the low 8 result bits
 * 
 * The 5 leading zeros in tx byte 0 are padding so the 12-bit result ends exactly on the last byte.
 */
#include "mcp3208.h"
#include "r5cap_fastmem.h"

#define START_BIT 0x04
#define SINGLE_ENDED_BIT 0x02
#define CHANNEL_HIGH_BIT 0x04     // D2, sent in byte 0
#define CHANNEL_HIGH_SHIFT 2
#define CHANNEL_LOW_BITS 0x03     // D1 D0, sent at the top of byte 1
#define CHANNEL_LOW_SHIFT 6
#define RESULT_HIGH_MASK 0x0F     // B11-B8 in the low nibble of rx byte 1
#define BITS_PER_BYTE 8

R5CAP_FAST_CODE bool MCP3208_encodeCommand(uint8_t channel, uint8_t tx[MCP3208_TRANSFER_BYTES])
{
    if (channel >= MCP3208_NUM_CHANNELS) {
        return false;
    }
    tx[0] = (uint8_t)(START_BIT | SINGLE_ENDED_BIT |
                      ((channel & CHANNEL_HIGH_BIT) >> CHANNEL_HIGH_SHIFT));
    tx[1] = (uint8_t)((channel & CHANNEL_LOW_BITS) << CHANNEL_LOW_SHIFT);
    tx[2] = 0;
    return true;
}

R5CAP_FAST_CODE uint16_t MCP3208_decodeResult(const uint8_t rx[MCP3208_TRANSFER_BYTES])
{
    return (uint16_t)(((rx[1] & RESULT_HIGH_MASK) << BITS_PER_BYTE) | rx[2]);
}
