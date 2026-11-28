// Shared between the heartbeat firmware and its Linux reader
#ifndef R5_HEARTBEAT_H
#define R5_HEARTBEAT_H

#include <stdint.h>

// Top 1 MB of the MCU R5F's reserved memory (0xA1100000, 15 MB)
#define R5_HEARTBEAT_ADDR 0xA1F00000u
#define R5_HEARTBEAT_MAGIC 0x54414548u   // "HEAT"
#define R5_HEARTBEAT_PERIOD_MS 10

struct r5_heartbeat {
	uint32_t magic;
	uint32_t boots;
	uint32_t count;
	uint32_t uptimeMs;
};

#endif
