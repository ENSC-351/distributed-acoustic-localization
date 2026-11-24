// capture.c
// See capture.h. Ring layout and the publishing protocol are in r5cap_shared.h.
#include "capture.h"
#include "mcp3208.h"
#include "r5_hal.h"
#include <string.h>

// A tick is late if more than 1.5 frame periods passed since the previous one
#define LATE_RATIO_NUM 3
#define LATE_RATIO_DEN 2
#define MIN_RING_BLOCKS 2
#define REGION_ALIGNMENT 8

static void *region = NULL;
static struct r5cap_header *header = NULL;
static struct CaptureConfig config;
static bool isRunning = false;

static uint32_t blockNumber = 0;     // block being filled
static uint32_t frameInBlock = 0;    // next frame to fill in that block
static uint64_t blockFirstTick = 0;  // when that block's frame 0 started
static uint32_t blockFlags = 0;      // R5CAP_BLOCK_* for that block
static bool hasPreviousTick = false;
static uint64_t previousTickStart = 0;

static bool isPowerOfTwo(uint32_t x)
{
    return x != 0 && (x & (x - 1)) == 0;
}

static bool isValidConfig(const struct CaptureConfig *c)
{
    if (c->numChannels < 1 || c->numChannels > R5CAP_MAX_CHANNELS) {
        return false;
    }
    for (uint32_t i = 0; i < c->numChannels; i++) {
        if (c->channels[i] >= MCP3208_NUM_CHANNELS) {
            return false;
        }
    }
    return c->blockFrames >= 1 && c->ringBlocks >= MIN_RING_BLOCKS &&
           isPowerOfTwo(c->ringBlocks) && c->framePeriodTicks > 0;
}

// One MCP3208 conversion through the HAL
static bool convertChannel(uint8_t channel, uint16_t *value)
{
    uint8_t tx[MCP3208_TRANSFER_BYTES];
    uint8_t rx[MCP3208_TRANSFER_BYTES];
    if (!MCP3208_encodeCommand(channel, tx) ||
        !R5HAL_spiTransfer(tx, rx, MCP3208_TRANSFER_BYTES)) {
        return false;
    }
    *value = MCP3208_decodeResult(rx);
    return true;
}

// One throwaway frame, timing when each conversion starts relative to mic 0.
// This is the inter-mic skew the estimator corrects, so readers need it in the
// header before the first block arrives.
static bool measureChannelOffsets(void)
{
    uint64_t frameStart = R5HAL_nowTicks();
    for (uint32_t i = 0; i < config.numChannels; i++) {
        uint64_t start = R5HAL_nowTicks();
        uint16_t unused = 0;
        if (!convertChannel(config.channels[i], &unused)) {
            return false;
        }
        header->channelOffsetTicks[i] = (uint32_t)(start - frameStart);
    }
    return true;
}

bool CAPTURE_init(void *sharedRegion, size_t regionSize, const struct CaptureConfig *userConfig)
{
    isRunning = false;
    if (sharedRegion == NULL || userConfig == NULL || !isValidConfig(userConfig) ||
        ((uintptr_t)sharedRegion % REGION_ALIGNMENT) != 0) {
        return false;
    }
    size_t needed = r5cap_regionSize(userConfig->numChannels, userConfig->blockFrames,
                                     userConfig->ringBlocks);
    if (regionSize < needed) {
        return false;
    }

    config = *userConfig;
    region = sharedRegion;
    header = (struct r5cap_header *)sharedRegion;
    memset(region, 0, needed);   // also sets state = STOPPED while we set up

    header->magic = R5CAP_MAGIC;
    header->version = R5CAP_VERSION;
    header->numChannels = config.numChannels;
    header->blockFrames = config.blockFrames;
    header->ringBlocks = config.ringBlocks;
    header->tickHz = R5HAL_tickHz();
    header->framePeriodTicks = config.framePeriodTicks;
    for (uint32_t i = 0; i < config.numChannels; i++) {
        header->channels[i] = config.channels[i];
    }

    // No slot holds a valid block yet. Leaving seq at 0 would look like
    // "this slot holds block 0", so mark every slot as being written.
    for (uint32_t slot = 0; slot < config.ringBlocks; slot++) {
        r5cap_meta(region, slot)->seq = R5CAP_SEQ_WRITING;
    }

    if (!measureChannelOffsets()) {
        header->state = R5CAP_STATE_ERROR;
        return false;
    }

    blockNumber = config.firstBlockNumber;
    frameInBlock = 0;
    blockFlags = 0;
    hasPreviousTick = false;
    header->writeSeq = blockNumber;

    R5CAP_BARRIER();   // everything above is visible before readers see RUNNING
    header->state = R5CAP_STATE_RUNNING;
    isRunning = true;
    return true;
}

// Frame 0 of a new block: take over its slot
static void startBlock(uint32_t slot, uint64_t now)
{
    blockFirstTick = now;
    blockFlags = 0;
    // Readers must stop trusting the old block in this slot before we overwrite it
    r5cap_meta(region, slot)->seq = R5CAP_SEQ_WRITING;
    R5CAP_BARRIER();
}

// Flags this block if the tick came more than 1.5 periods after the previous one
static void checkTickTiming(uint64_t now)
{
    if (hasPreviousTick) {
        uint64_t elapsed = now - previousTickStart;
        uint64_t period = config.framePeriodTicks;
        if (elapsed * LATE_RATIO_DEN > period * LATE_RATIO_NUM) {
            // Whole frames that never happened (rounded); at least 1
            uint64_t missed = (elapsed + period / 2) / period - 1;
            header->lateTicks += (uint32_t)(missed > 0 ? missed : 1);
            blockFlags |= R5CAP_BLOCK_LATE_TICK;
        }
    }
    hasPreviousTick = true;
    previousTickStart = now;
}

// Last frame of a block done: make it visible to readers (seqlock, see r5cap_shared.h)
static void publishBlock(uint32_t slot)
{
    struct r5cap_block_meta *meta = r5cap_meta(region, slot);
    meta->firstTick = blockFirstTick;
    meta->flags = blockFlags;
    R5CAP_BARRIER();          // samples + meta written before the slot is marked valid
    meta->seq = blockNumber;
    R5CAP_BARRIER();          // slot valid before readers are told the block exists
    blockNumber++;
    header->writeSeq = blockNumber;
    frameInBlock = 0;
}

void CAPTURE_tick(void)
{
    if (!isRunning) {
        return;
    }
    uint64_t now = R5HAL_nowTicks();
    uint32_t slot = blockNumber & (config.ringBlocks - 1);
    if (frameInBlock == 0) {
        startBlock(slot, now);   // resets blockFlags, so it must run before checkTickTiming
    }
    checkTickTiming(now);

    uint16_t *frame = r5cap_slotSamples(region, slot) + (size_t)frameInBlock * config.numChannels;
    for (uint32_t i = 0; i < config.numChannels; i++) {
        if (!convertChannel(config.channels[i], &frame[i])) {
            frame[i] = 0;
            blockFlags |= R5CAP_BLOCK_ADC_ERROR;
            header->adcErrors++;
        }
    }

    frameInBlock++;
    if (frameInBlock == config.blockFrames) {
        publishBlock(slot);
    }
}

void CAPTURE_stop(void)
{
    isRunning = false;
    if (header != NULL) {
        R5CAP_BARRIER();   // the last writeSeq is visible before STOPPED (readers rely on this)
        header->state = R5CAP_STATE_STOPPED;
    }
}
