// r5cap_heartbeat.h
// Phase 1 "is the R5 running OUR code" check: the firmware writes this small
// struct at the start of the r5capture shared region and updates it every
// R5CAP_HEARTBEAT_PERIOD_MS. Linux maps the same physical address and checks
// that the counter moves. Phase 2 puts the capture ring in this region instead.
//
// Both the firmware (zephyr_app/) and the Linux tool (linux/r5cap_hb_read.c)
// compile this header.
#ifndef R5CAP_HEARTBEAT_H
#define R5CAP_HEARTBEAT_H

#include <stdint.h>

// Shared region: the top 1 MB of the MCU R5F's 15 MB DDR carve-out
// (mcu-r5fss-memory-region@a1100000). The firmware's device-tree overlay
// shrinks its own DRAM so it never uses this range.
#define R5CAP_SHM_PHYS 0xA1F00000u
#define R5CAP_SHM_SIZE 0x00100000u

#define R5CAP_HEARTBEAT_MAGIC 0x54424835u   // "5HBT" in little-endian memory
#define R5CAP_HEARTBEAT_VERSION 1u
#define R5CAP_HEARTBEAT_PERIOD_MS 100u

struct r5cap_heartbeat {
    uint32_t magic;               // R5CAP_HEARTBEAT_MAGIC once the firmware has started
    uint32_t version;             // R5CAP_HEARTBEAT_VERSION
    volatile uint32_t counter;    // +1 every R5CAP_HEARTBEAT_PERIOD_MS
    volatile uint32_t uptimeMs;   // R5's uptime when counter was last written
};

#endif
