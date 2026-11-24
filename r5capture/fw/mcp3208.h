/**
 * @file mcp3208.h
 * @author Madden Wynans
 * @brief Builds the SPI command for one MCP3208 conversion and decodes the reply
 * @version 0.1
 * @date 2026-11-24
 * @copyright Copyright (c) 2026
 */
#ifndef MCP3208_H
#define MCP3208_H

#include <stdbool.h>
#include <stdint.h>

#define MCP3208_NUM_CHANNELS 8
#define MCP3208_MAX_READING 4095
#define MCP3208_TRANSFER_BYTES 3

/**
 * @brief Fills TX with the command for a single-ended convresion of channel (0.7)
 * @return false if the channel is out of range
 */
bool MCP3208_encodeCommand(uint8_t channel, uint8_t tx[MCP3208_TRANSFER_BYTES]);

/**
 * @brief Returns the 12 bit result from the bytes received during transfer
 */
uint16_t MCP3208_decodeResult(const uint8_t rx[MCP3208_TRANSFER_BYTES]);
#endif