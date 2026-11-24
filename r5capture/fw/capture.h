// capture.h
// The capture core: on every frame tick, convert each microphone's ADC channel
// once and store the samples in the shared-memory ring (r5cap_shared.h),
// publishing each block when it is full.
//
// Portable: uses only r5_hal.h for hardware, so the same code runs on the R5
// and in unit tests on a PC.
//
// Who calls what:
//   CAPTURE_init()  once at startup, before the frame timer starts
//   CAPTURE_tick()  once per frame, from the timer ISR (or a polling loop).
//                   Must finish before the next tick.
//   CAPTURE_stop()  to stop; later ticks do nothing
#ifndef CAPTURE_H
#define CAPTURE_H

#include "r5cap_shared.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct CaptureConfig {
    uint32_t numChannels;                    // 1 to R5CAP_MAX_CHANNELS
    uint8_t channels[R5CAP_MAX_CHANNELS];    // ADC channel of each mic, in frame order
    uint32_t blockFrames;                    // frames per block
    uint32_t ringBlocks;                     // slots in the ring; a power of two, at least 2
    uint32_t framePeriodTicks;               // time between ticks, in R5HAL_tickHz() ticks
    uint32_t firstBlockNumber;               // normally 0; tests set it near the 32-bit wrap
};

// Lays out the shared region, writes the header, and measures the time between
// channel conversions (one throwaway frame). region must hold at least
// r5cap_regionSize(numChannels, blockFrames, ringBlocks) bytes and be 8-byte aligned.
// Returns false on a bad config, a region too small, or a failed ADC transfer.
bool CAPTURE_init(void *region, size_t regionSize, const struct CaptureConfig *config);

// Converts one frame (every channel once). Call once per frame period.
void CAPTURE_tick(void);

// Marks the capture stopped; ticks after this do nothing.
void CAPTURE_stop(void);

#endif
