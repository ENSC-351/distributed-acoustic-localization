// r5cap_shared.h
// Shared-memory layout between the R5F capture firmware (the only writer) and
// Linux (any number of readers). Both sides compile this same header, so it
// uses fixed-width types only, no pointers, and explicit padding.
//
// Region layout (all offsets from the start of the region):
//   struct r5cap_header                       (R5CAP_HEADER_SIZE bytes)
//   struct r5cap_block_meta[ring_blocks]      (one per ring slot)
//   uint16_t samples[ring_blocks][block_frames * num_channels]
//        frame by frame inside a block: [f0 ch0, f0 ch1, f1 ch0, f1 ch1, ...]
//
// No locks. Each slot carries the number of the block it holds (a seqlock):
//   writer: meta.seq = R5CAP_SEQ_WRITING; barrier; write samples + meta;
//           barrier; meta.seq = block number; barrier; header.write_seq++
//   reader: s1 = meta.seq; barrier; copy; barrier; s2 = meta.seq;
//           the copy is good only if s1 == s2 == the block it wanted.
// The writer never waits for readers; a reader that falls behind loses blocks
// and is told how many.
//
// Known limitation: block number 0xFFFFFFFF equals R5CAP_SEQ_WRITING, so a
// reader can't tell "holds block 0xFFFFFFFF" from "being written". Readers
// always skip that one block (once every ~318 days at 20 kHz / 128-frame blocks).
//
// The R5 and A53 caches are not coherent: map the region uncached on both
// sides (R5: MPU region non-cacheable; Linux: mmap with O_SYNC), or clean the
// R5 data cache before publishing a block.
#ifndef R5CAP_SHARED_H
#define R5CAP_SHARED_H

#include <stddef.h>
#include <stdint.h>

#define R5CAP_MAGIC 0x50414335u      // "5CAP" in little-endian memory
#define R5CAP_VERSION 1u
#define R5CAP_MAX_CHANNELS 8
#define R5CAP_SEQ_WRITING 0xFFFFFFFFu  // slot is being rewritten
#define R5CAP_HEADER_SIZE 128u

// Barrier strong enough for two non-coherent cores sharing uncached memory
#if defined(__aarch64__)
#define R5CAP_BARRIER() __asm__ volatile("dmb sy" ::: "memory")
#elif defined(__arm__)
#define R5CAP_BARRIER() __asm__ volatile("dmb" ::: "memory")
#else
#define R5CAP_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

enum r5cap_state {
    R5CAP_STATE_STOPPED = 0,
    R5CAP_STATE_RUNNING = 1,
    R5CAP_STATE_ERROR = 2,
};

// r5cap_block_meta.flags
#define R5CAP_BLOCK_LATE_TICK (1u << 0)  // a frame tick ran late or was missed inside this
                                         // block: frames may not be evenly spaced
#define R5CAP_BLOCK_ADC_ERROR (1u << 1)  // an SPI transfer failed; affected samples are 0

struct r5cap_header {
    // Written once by the firmware before state becomes RUNNING
    uint32_t magic;
    uint32_t version;
    uint32_t numChannels;
    uint32_t blockFrames;
    uint32_t ringBlocks;                 // power of two
    uint32_t tickHz;                     // timer frequency the tick values below are in
    uint32_t framePeriodTicks;           // exact time between frames (timer-driven)
    uint8_t channels[R5CAP_MAX_CHANNELS];         // ADC channel of each mic, in frame order
    uint32_t channelOffsetTicks[R5CAP_MAX_CHANNELS]; // how long after mic 0 each mic is sampled
    // Updated while running
    volatile uint32_t state;             // enum r5cap_state
    volatile uint32_t writeSeq;          // next block number to be published (starts at the first block
                                         //   number, normally 0; wraps, so compare with subtraction)
    volatile uint32_t lateTicks;         // frame ticks that ran late or were missed
    volatile uint32_t adcErrors;         // failed SPI transfers
    uint8_t reserved[R5CAP_HEADER_SIZE - 84];
};

struct r5cap_block_meta {
    volatile uint32_t seq;               // block number held in this slot, or R5CAP_SEQ_WRITING
    uint32_t flags;                      // R5CAP_BLOCK_*
    uint64_t firstTick;                  // timer tick when frame 0's first conversion started
};

_Static_assert(sizeof(struct r5cap_header) == R5CAP_HEADER_SIZE, "header size is part of the ABI");
_Static_assert(sizeof(struct r5cap_block_meta) == 16, "block meta size is part of the ABI");

static inline size_t r5cap_samplesPerBlock(uint32_t numChannels, uint32_t blockFrames)
{
    return (size_t)numChannels * blockFrames;
}

static inline size_t r5cap_regionSize(uint32_t numChannels, uint32_t blockFrames, uint32_t ringBlocks)
{
    return R5CAP_HEADER_SIZE + (size_t)ringBlocks * sizeof(struct r5cap_block_meta) +
           (size_t)ringBlocks * r5cap_samplesPerBlock(numChannels, blockFrames) * sizeof(uint16_t);
}

static inline struct r5cap_block_meta *r5cap_meta(void *region, uint32_t slot)
{
    return (struct r5cap_block_meta *)((uint8_t *)region + R5CAP_HEADER_SIZE) + slot;
}

static inline uint16_t *r5cap_slotSamples(void *region, uint32_t slot)
{
    const struct r5cap_header *h = (const struct r5cap_header *)region;
    uint8_t *samples = (uint8_t *)region + R5CAP_HEADER_SIZE +
                       (size_t)h->ringBlocks * sizeof(struct r5cap_block_meta);
    return (uint16_t *)samples + (size_t)slot * r5cap_samplesPerBlock(h->numChannels, h->blockFrames);
}

#endif
