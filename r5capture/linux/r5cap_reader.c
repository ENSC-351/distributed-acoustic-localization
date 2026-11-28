// r5cap_reader.c
// See r5cap_reader.h. Implements the reader half of the seqlock described in
// r5cap_shared.h.
#include "r5cap_reader.h"
#include "r5cap_shared.h"

static bool isPowerOfTwo(uint32_t x)
{
    return x != 0 && (x & (x - 1)) == 0;
}

bool R5CAPREADER_open(struct R5CapReader *reader, void *region, size_t regionSize)
{
    if (reader == NULL || region == NULL || regionSize < R5CAP_HEADER_SIZE) {
        return false;
    }
    const struct r5cap_header *header = (const struct r5cap_header *)region;
    if (header->magic != R5CAP_MAGIC || header->version != R5CAP_VERSION ||
        header->numChannels < 1 || header->numChannels > R5CAP_MAX_CHANNELS ||
        header->blockFrames < 1 || !isPowerOfTwo(header->ringBlocks) ||
        regionSize < r5cap_regionSize(header->numChannels, header->blockFrames,
                                      header->ringBlocks)) {
        return false;
    }
    reader->region = region;
    reader->nextBlock = header->writeSeq;
    reader->pendingSkipped = 0;
    return true;
}

// Copy one slot's samples. Uses 16-bit loads instead of memcpy: on the board
// the region is mapped uncached, and memcpy may use unaligned or wide accesses
// that aren't allowed on device memory.
static void copySamples(uint16_t *dest, const volatile uint16_t *src, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        dest[i] = src[i];
    }
}

enum R5CapReadResult R5CAPREADER_read(struct R5CapReader *reader, uint16_t *samples,
                                      struct R5CapBlockInfo *info)
{
    struct r5cap_header *header = (struct r5cap_header *)reader->region;
    const uint32_t ringBlocks = header->ringBlocks;
    const size_t perBlock = r5cap_samplesPerBlock(header->numChannels, header->blockFrames);

    for (;;) {
        // State first, then writeSeq. The writer publishes its last block before
        // setting STOPPED, so if we see STOPPED here, the writeSeq we read next is
        // final. (Reading them the other way round could see an old writeSeq and
        // a new STOPPED, and quit with blocks still unread.)
        uint32_t state = header->state;
        R5CAP_BARRIER();
        uint32_t writeSeq = header->writeSeq;
        R5CAP_BARRIER();   // read writeSeq before anything it announces

        if (writeSeq == reader->nextBlock) {
            return (state == R5CAP_STATE_RUNNING) ? R5CAP_READ_NONE : R5CAP_READ_STOPPED;
        }

        // Fell too far behind: the oldest blocks are already overwritten. The
        // slot after the newest published block may be mid-rewrite, so only
        // ringBlocks - 1 blocks are safely kept.
        uint32_t available = writeSeq - reader->nextBlock;   // unsigned: handles the wrap
        if (available > ringBlocks - 1) {
            uint32_t lost = available - (ringBlocks - 1);
            reader->pendingSkipped += lost;
            reader->nextBlock += lost;
        }

        uint32_t wanted = reader->nextBlock;
        if (wanted == R5CAP_SEQ_WRITING) {
            // Can't be told apart from "being written": always skipped (see r5cap_shared.h)
            reader->pendingSkipped++;
            reader->nextBlock++;
            continue;
        }

        uint32_t slot = wanted & (ringBlocks - 1);
        struct r5cap_block_meta *meta = r5cap_meta(reader->region, slot);

        uint32_t seqBefore = meta->seq;
        R5CAP_BARRIER();   // check the slot before copying it
        if (seqBefore != wanted) {
            // Already overwritten by a newer block (the R5 lapped us)
            reader->pendingSkipped++;
            reader->nextBlock++;
            continue;
        }

        copySamples(samples, r5cap_slotSamples(reader->region, slot), perBlock);
        uint64_t firstTick = meta->firstTick;
        uint32_t flags = meta->flags;
        R5CAP_BARRIER();   // finish copying before re-checking the slot
        uint32_t seqAfter = meta->seq;
        if (seqAfter != wanted) {
            // The R5 started rewriting the slot while we copied: the copy may be torn
            reader->pendingSkipped++;
            reader->nextBlock++;
            continue;
        }

        info->blockNumber = wanted;
        info->firstTick = firstTick;
        info->flags = flags;
        info->skipped = reader->pendingSkipped;
        reader->pendingSkipped = 0;
        reader->nextBlock++;
        return R5CAP_READ_OK;
    }
}
