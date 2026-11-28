// r5cap_reader.h
// Linux side of the shared-memory ring (r5cap_shared.h): reads the blocks the
// R5 capture firmware publishes, in order, and reports any it missed.
//
// Each reader keeps its own position, so several readers (DSP, meter,
// recorder) can read the same ring independently. A reader must only be used
// by one thread. Reading never blocks and never slows the R5 down.
//
// Later this becomes the R5 backend inside audio/src/audio_capture.c.
#ifndef R5CAP_READER_H
#define R5CAP_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct R5CapReader {
    void *region;
    uint32_t nextBlock;       // block number this reader wants next
    uint32_t pendingSkipped;  // blocks lost since the last block returned
};

struct R5CapBlockInfo {
    uint32_t blockNumber;
    uint64_t firstTick;       // R5 timer tick of frame 0's first conversion
    uint32_t flags;           // R5CAP_BLOCK_*
    uint32_t skipped;         // blocks this reader missed just before this one
};

enum R5CapReadResult {
    R5CAP_READ_OK,            // a block was copied
    R5CAP_READ_NONE,          // no new block yet; try again later
    R5CAP_READ_STOPPED,       // capture is not running and no blocks are left
};

// Checks the region (magic, version, sizes) and starts the reader at the next
// block the R5 will publish. Returns false if the region isn't a valid ring.
bool R5CAPREADER_open(struct R5CapReader *reader, void *region, size_t regionSize);

// Copies the reader's next block into samples (room for numChannels * blockFrames
// values, frame by frame: [f0 ch0, f0 ch1, f1 ch0, ...]). Never blocks.
enum R5CapReadResult R5CAPREADER_read(struct R5CapReader *reader, uint16_t *samples,
                                      struct R5CapBlockInfo *info);

#endif
