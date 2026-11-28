// r5cap_stream.h
// Wire format of linux/r5cap_stream.c (little-endian, as the board and PCs are).
// On connect: one r5cap_stream_header. Then, per block: one r5cap_stream_block
// followed by numChannels * blockFrames uint16_t samples, frame by frame.
// tools/live_spectrum.py reads this; keep the two in step.
#ifndef R5CAP_STREAM_H
#define R5CAP_STREAM_H

#include <stdint.h>

#define R5CAP_STREAM_MAGIC 0x54533552u   // "R5ST"
#define R5CAP_STREAM_VERSION 1u
#define R5CAP_STREAM_PORT 35200

struct r5cap_stream_header {
    uint32_t magic;
    uint32_t version;
    uint32_t numChannels;
    uint32_t blockFrames;
    uint32_t tickHz;
    uint32_t framePeriodTicks;     // sample rate = tickHz / framePeriodTicks
    uint32_t channel1OffsetTicks;  // how long after mic 0 mic 1 is sampled
    uint32_t reserved;
};

struct r5cap_stream_block {
    uint32_t blockNumber;
    uint32_t flags;                // R5CAP_BLOCK_*
    uint32_t skipped;              // blocks lost just before this one
    uint32_t reserved;
};

_Static_assert(sizeof(struct r5cap_stream_header) == 32, "header size is part of the format");
_Static_assert(sizeof(struct r5cap_stream_block) == 16, "block size is part of the format");

#endif
