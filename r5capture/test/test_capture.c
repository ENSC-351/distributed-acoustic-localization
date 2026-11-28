// test_capture.c
// The capture core on its own, driven by the simulated hardware (hal_sim):
// init checks, block filling and publishing, late ticks (T0.5), ADC errors, stop.
#include "capture.h"
#include "hal_sim.h"
#include "test_check.h"
#include <stdbool.h>
#include <string.h>

#define PERIOD 50        // ticks between frames (50 us with SIM_TICK_HZ = 1 MHz)
#define CONVERSION 20    // ticks per SPI transfer, so mic 1 is sampled 20 ticks after mic 0
#define BASE_TICK 1000   // time of the first tick
#define FILL_BYTE 0xA5   // region is filled with this before init, to catch fields init forgets

// Raw bytes for the shared region, 8-byte aligned like the real carve-out
static _Alignas(8) uint8_t region[32768];
static uint64_t tickIndex = 0;   // ticks run since startCapture()
static uint64_t extraDelay = 0;  // total delay injected so far

static struct r5cap_header *header(void)
{
    return (struct r5cap_header *)region;
}

static struct CaptureConfig defaultConfig(void)
{
    return (struct CaptureConfig){
        .numChannels = 2,
        .channels = {0, 1},
        .blockFrames = 8,
        .ringBlocks = 4,
        .framePeriodTicks = PERIOD,
    };
}

static void startCapture(const struct CaptureConfig *config)
{
    SIM_reset();
    SIM_setConversionTicks(CONVERSION);
    memset(region, FILL_BYTE, sizeof(region));
    CHECK(CAPTURE_init(region, sizeof(region), config));
    tickIndex = 0;
    extraDelay = 0;
}

// Runs ticks exactly one period apart (plus any injected delay), like a timer ISR
static void runTicks(uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        SIM_setTime(BASE_TICK + tickIndex * PERIOD + extraDelay);
        CAPTURE_tick();
        tickIndex++;
    }
}

// The next tick (and every one after) comes this much later than scheduled
static void delayNextTick(uint64_t ticks)
{
    extraDelay += ticks;
}

// True if every sample in the slot equals the simulated signal at the moment
// it was converted: frame f, mic c sampled at firstTick + f * PERIOD + offset[c].
// Only valid for blocks whose ticks were evenly spaced.
static bool slotMatchesSignal(uint32_t slot)
{
    const struct r5cap_block_meta *meta = r5cap_meta(region, slot);
    const uint16_t *samples = r5cap_slotSamples(region, slot);
    const struct r5cap_header *h = header();
    for (uint32_t f = 0; f < h->blockFrames; f++) {
        for (uint32_t c = 0; c < h->numChannels; c++) {
            uint64_t sampledAt = meta->firstTick + (uint64_t)f * PERIOD + h->channelOffsetTicks[c];
            if (samples[f * h->numChannels + c] != SIM_defaultSignal(h->channels[c], sampledAt)) {
                return false;
            }
        }
    }
    return true;
}

static void testInitRejectsBadConfigs(void)
{
    SIM_reset();
    struct CaptureConfig bad[8];
    for (int i = 0; i < 8; i++) {
        bad[i] = defaultConfig();
    }
    bad[0].numChannels = 0;
    bad[1].numChannels = R5CAP_MAX_CHANNELS + 1;
    bad[2].channels[1] = 8;           // the MCP3208 has channels 0-7
    bad[3].ringBlocks = 3;            // not a power of two
    bad[4].ringBlocks = 1;            // too few
    bad[5].framePeriodTicks = 0;
    bad[6].blockFrames = 0;
    for (int i = 0; i < 7; i++) {
        memset(region, FILL_BYTE, sizeof(region));
        CHECK(!CAPTURE_init(region, sizeof(region), &bad[i]));
        CHECK(region[0] == FILL_BYTE);   // rejected before touching memory
    }

    struct CaptureConfig good = defaultConfig();
    size_t needed = r5cap_regionSize(good.numChannels, good.blockFrames, good.ringBlocks);
    CHECK(!CAPTURE_init(region, needed - 1, &good));                       // too small
    CHECK(!CAPTURE_init(region + 4, sizeof(region) - 4, &good));            // misaligned
    CHECK(!CAPTURE_init(NULL, sizeof(region), &good));
    CHECK(!CAPTURE_init(region, sizeof(region), NULL));
    CHECK(CAPTURE_init(region, needed, &good));                            // exactly enough
}

static void testInitWritesHeader(void)
{
    struct CaptureConfig config = defaultConfig();
    startCapture(&config);
    const struct r5cap_header *h = header();
    CHECK(h->magic == R5CAP_MAGIC);
    CHECK(h->version == R5CAP_VERSION);
    CHECK(h->numChannels == 2);
    CHECK(h->blockFrames == 8);
    CHECK(h->ringBlocks == 4);
    CHECK(h->tickHz == SIM_TICK_HZ);
    CHECK(h->framePeriodTicks == PERIOD);
    CHECK(h->channels[0] == 0 && h->channels[1] == 1);
    // Measured, not assumed: mic 1 is converted one transfer after mic 0
    CHECK(h->channelOffsetTicks[0] == 0);
    CHECK(h->channelOffsetTicks[1] == CONVERSION);
    CHECK(h->state == R5CAP_STATE_RUNNING);
    CHECK(h->writeSeq == 0);
    CHECK(h->lateTicks == 0 && h->adcErrors == 0);
    for (uint32_t slot = 0; slot < config.ringBlocks; slot++) {
        CHECK(r5cap_meta(region, slot)->seq == R5CAP_SEQ_WRITING);   // no slot claims a block yet
    }
}

static void testBlocksAreFilledAndPublished(void)
{
    struct CaptureConfig config = defaultConfig();
    startCapture(&config);
    runTicks(10 * config.blockFrames);   // 10 blocks through a 4-slot ring
    CHECK(header()->writeSeq == 10);
    // The ring holds the newest 4 blocks: 6, 7, 8, 9
    for (uint32_t block = 6; block < 10; block++) {
        uint32_t slot = block & (config.ringBlocks - 1);
        const struct r5cap_block_meta *meta = r5cap_meta(region, slot);
        CHECK(meta->seq == block);
        CHECK(meta->firstTick == BASE_TICK + (uint64_t)block * config.blockFrames * PERIOD);
        CHECK(meta->flags == 0);
        CHECK(slotMatchesSignal(slot));
    }
    CHECK(SIM_getProtocolErrors() == 0);
    CHECK(header()->lateTicks == 0);
}

static void testPartialBlockIsNotPublished(void)
{
    struct CaptureConfig config = defaultConfig();
    startCapture(&config);
    runTicks(config.blockFrames + 3);   // block 0 done, block 1 three frames in
    CHECK(header()->writeSeq == 1);
    CHECK(r5cap_meta(region, 0)->seq == 0);
    CHECK(r5cap_meta(region, 1)->seq == R5CAP_SEQ_WRITING);
}

// T0.5: ticks that come late are counted and flag exactly the right blocks
static void testLateTicks(void)
{
    struct CaptureConfig config = defaultConfig();
    config.ringBlocks = 8;
    startCapture(&config);

    runTicks(20);                  // blocks 0, 1 and frames 0-3 of block 2
    delayNextTick(2 * PERIOD);     // frame 4 of block 2 comes 3 periods after frame 3: 2 frames missed
    runTicks(12);                  // rest of block 2, all of block 3
    delayNextTick(PERIOD * 2 / 5); // 1.4 periods: a bit late, but under the 1.5 limit
    runTicks(8);                   // block 4
    delayNextTick(PERIOD * 7 / 10);// 1.7 periods, on frame 0 of block 5: 1 frame counted missed
    runTicks(16);                  // blocks 5, 6

    CHECK(header()->writeSeq == 7);
    CHECK(header()->lateTicks == 3);
    const uint32_t expectedLate[7] = {0, 0, 1, 0, 0, 1, 0};
    for (uint32_t block = 0; block < 7; block++) {
        uint32_t flags = r5cap_meta(region, block)->flags;
        CHECK(((flags & R5CAP_BLOCK_LATE_TICK) != 0) == (expectedLate[block] != 0));
    }
    // Blocks with evenly spaced ticks still hold the right samples
    CHECK(slotMatchesSignal(3));
    CHECK(slotMatchesSignal(6));
}

static void testAdcErrorsAreFlagged(void)
{
    struct CaptureConfig config = defaultConfig();
    startCapture(&config);
    runTicks(config.blockFrames);       // block 0: clean
    SIM_failNextTransfers(1);           // block 1, frame 0, mic 0 fails
    runTicks(2 * config.blockFrames);   // blocks 1 and 2
    CHECK(header()->adcErrors == 1);
    CHECK(r5cap_meta(region, 0)->flags == 0);
    CHECK(r5cap_meta(region, 1)->flags == R5CAP_BLOCK_ADC_ERROR);
    CHECK(r5cap_meta(region, 2)->flags == 0);
    CHECK(r5cap_slotSamples(region, 1)[0] == 0);   // the failed sample
    CHECK(slotMatchesSignal(2));
}

// The frame loop can report schedule slips that CAPTURE_tick() can't see
static void testMarkLate(void)
{
    struct CaptureConfig config = defaultConfig();
    startCapture(&config);
    runTicks(config.blockFrames);       // block 0: clean
    runTicks(3);                        // block 1, frames 0-2
    CAPTURE_markLate(0);                // started late, nothing missed
    CAPTURE_markLate(2);                // missed 2 frame slots
    runTicks(config.blockFrames - 3);   // rest of block 1
    runTicks(config.blockFrames);       // block 2: clean
    CHECK(header()->lateTicks == 3);
    CHECK(r5cap_meta(region, 0)->flags == 0);
    CHECK(r5cap_meta(region, 1)->flags == R5CAP_BLOCK_LATE_TICK);
    CHECK(r5cap_meta(region, 2)->flags == 0);
}

// A late frame 0 must flag the block it starts, not the one before
static void testMarkLateOnFirstFrame(void)
{
    struct CaptureConfig config = defaultConfig();
    startCapture(&config);
    runTicks(config.blockFrames);       // block 0: clean
    CAPTURE_markLate(0);                // frame 0 of block 1 starts late
    runTicks(2 * config.blockFrames);   // blocks 1 and 2
    CHECK(header()->lateTicks == 1);
    CHECK(r5cap_meta(region, 0)->flags == 0);
    CHECK(r5cap_meta(region, 1)->flags == R5CAP_BLOCK_LATE_TICK);
    CHECK(r5cap_meta(region, 2)->flags == 0);
}

static void testStop(void)
{
    struct CaptureConfig config = defaultConfig();
    startCapture(&config);
    runTicks(config.blockFrames);
    CAPTURE_stop();
    CHECK(header()->state == R5CAP_STATE_STOPPED);
    uint32_t transfers = SIM_getTransferCount();
    runTicks(2 * config.blockFrames);
    CHECK(header()->writeSeq == 1);
    CHECK(SIM_getTransferCount() == transfers);   // no conversions after stop
}

int main(void)
{
    testInitRejectsBadConfigs();
    testInitWritesHeader();
    testBlocksAreFilledAndPublished();
    testPartialBlockIsNotPublished();
    testLateTicks();
    testAdcErrorsAreFlagged();
    testMarkLate();
    testMarkLateOnFirstFrame();
    testStop();
    return TEST_finish("test_capture");
}
