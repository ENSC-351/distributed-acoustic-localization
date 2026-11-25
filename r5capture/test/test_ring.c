// test_ring.c
// The capture core (writer) and the Linux reader together:
//   T0.2 reader keeps up, T0.3 slow reader, T0.4 torn reads with concurrent
//   threads, T0.6 block-number wrap, plus open checks and the stopped state.
#include "capture.h"
#include "hal_sim.h"
#include "r5cap_reader.h"
#include "test_check.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define PERIOD 50
#define CONVERSION 20
#define BASE_TICK 1000
#define MAX_BLOCK_SAMPLES 256

// Raw bytes for the shared region, 8-byte aligned like the real carve-out
static _Alignas(8) uint8_t region[65536];
static uint64_t tickIndex = 0;

static struct CaptureConfig ringConfig(uint32_t blockFrames, uint32_t ringBlocks)
{
    return (struct CaptureConfig){
        .numChannels = 2,
        .channels = {0, 1},
        .blockFrames = blockFrames,
        .ringBlocks = ringBlocks,
        .framePeriodTicks = PERIOD,
    };
}

static bool startCapture(const struct CaptureConfig *config)
{
    SIM_reset();
    SIM_setConversionTicks(CONVERSION);
    memset(region, 0xA5, sizeof(region));
    tickIndex = 0;
    return CAPTURE_init(region, sizeof(region), config);
}

static void runBlocks(uint32_t count)
{
    const struct r5cap_header *h = (const struct r5cap_header *)region;
    for (uint32_t i = 0; i < count * h->blockFrames; i++) {
        SIM_setTime(BASE_TICK + tickIndex * PERIOD);
        CAPTURE_tick();
        tickIndex++;
    }
}

// Every sample equals the simulated signal at the moment it was converted.
// Only uses the header's constant fields and the block's own info, so reader
// threads can call it.
static bool blockMatchesSignal(const uint16_t *samples, const struct R5CapBlockInfo *info)
{
    const struct r5cap_header *h = (const struct r5cap_header *)region;
    for (uint32_t f = 0; f < h->blockFrames; f++) {
        for (uint32_t c = 0; c < h->numChannels; c++) {
            uint64_t sampledAt = info->firstTick + (uint64_t)f * PERIOD + h->channelOffsetTicks[c];
            if (samples[f * h->numChannels + c] != SIM_defaultSignal(h->channels[c], sampledAt)) {
                return false;
            }
        }
    }
    return true;
}

static void testOpenValidates(void)
{
    struct R5CapReader reader;
    memset(region, 0, sizeof(region));
    CHECK(!R5CAPREADER_open(&reader, region, sizeof(region)));   // no capture yet: bad magic

    struct CaptureConfig config = ringConfig(16, 8);
    CHECK(startCapture(&config));
    size_t needed = r5cap_regionSize(2, 16, 8);
    CHECK(R5CAPREADER_open(&reader, region, needed));
    CHECK(!R5CAPREADER_open(&reader, region, needed - 1));
    ((struct r5cap_header *)region)->version = R5CAP_VERSION + 1;
    CHECK(!R5CAPREADER_open(&reader, region, needed));
}

// T0.2: a reader that keeps up gets every block, in order, intact
static void testReaderKeepsUp(void)
{
    struct CaptureConfig config = ringConfig(16, 8);
    CHECK(startCapture(&config));
    struct R5CapReader reader;
    CHECK(R5CAPREADER_open(&reader, region, sizeof(region)));

    uint16_t samples[MAX_BLOCK_SAMPLES];
    struct R5CapBlockInfo info;
    CHECK(R5CAPREADER_read(&reader, samples, &info) == R5CAP_READ_NONE);   // nothing yet

    const uint32_t blocks = 100000;
    uint32_t bad = 0;
    for (uint32_t n = 0; n < blocks; n++) {
        runBlocks(1);
        if (R5CAPREADER_read(&reader, samples, &info) != R5CAP_READ_OK ||
            info.blockNumber != n || info.skipped != 0 || info.flags != 0 ||
            info.firstTick != BASE_TICK + (uint64_t)n * config.blockFrames * PERIOD ||
            !blockMatchesSignal(samples, &info) ||
            R5CAPREADER_read(&reader, samples, &info) != R5CAP_READ_NONE) {
            bad++;
        }
    }
    CHECK(bad == 0);
    CHECK(SIM_getProtocolErrors() == 0);
}

// T0.3: a reader that falls more than a ring behind is told exactly how many
// blocks it lost, and everything it does get is intact
static void testSlowReader(void)
{
    struct CaptureConfig config = ringConfig(16, 8);
    CHECK(startCapture(&config));
    struct R5CapReader reader;
    CHECK(R5CAPREADER_open(&reader, region, sizeof(region)));

    runBlocks(26);   // blocks 0-25 while the reader sleeps
    uint16_t samples[MAX_BLOCK_SAMPLES];
    struct R5CapBlockInfo info;

    // Only ringBlocks - 1 = 7 blocks are safely kept: 19 to 25
    CHECK(R5CAPREADER_read(&reader, samples, &info) == R5CAP_READ_OK);
    CHECK(info.blockNumber == 19);
    CHECK(info.skipped == 19);
    CHECK(blockMatchesSignal(samples, &info));
    for (uint32_t n = 20; n < 26; n++) {
        CHECK(R5CAPREADER_read(&reader, samples, &info) == R5CAP_READ_OK);
        CHECK(info.blockNumber == n);
        CHECK(info.skipped == 0);
        CHECK(blockMatchesSignal(samples, &info));
    }
    CHECK(R5CAPREADER_read(&reader, samples, &info) == R5CAP_READ_NONE);
}

// T0.6: block numbers wrap from 0xFFFFFFFF to 0 without losing order.
// Block 0xFFFFFFFF itself is always skipped (see r5cap_shared.h).
static void testSequenceWrap(void)
{
    struct CaptureConfig config = ringConfig(16, 8);
    config.firstBlockNumber = 0xFFFFFFFFu - 5;
    CHECK(startCapture(&config));
    struct R5CapReader reader;
    CHECK(R5CAPREADER_open(&reader, region, sizeof(region)));

    uint16_t samples[MAX_BLOCK_SAMPLES];
    struct R5CapBlockInfo info;
    uint32_t got[16];
    uint32_t skippedAt[16];
    uint32_t count = 0;
    for (uint32_t i = 0; i < 12; i++) {   // blocks 0xFFFFFFFA ... 5
        runBlocks(1);
        while (R5CAPREADER_read(&reader, samples, &info) == R5CAP_READ_OK && count < 16) {
            CHECK(blockMatchesSignal(samples, &info));
            got[count] = info.blockNumber;
            skippedAt[count] = info.skipped;
            count++;
        }
    }
    const uint32_t expected[11] = {
        0xFFFFFFFAu, 0xFFFFFFFBu, 0xFFFFFFFCu, 0xFFFFFFFDu, 0xFFFFFFFEu,
        0, 1, 2, 3, 4, 5,
    };
    CHECK(count == 11);
    for (uint32_t i = 0; i < count && i < 11; i++) {
        CHECK(got[i] == expected[i]);
        CHECK(skippedAt[i] == (expected[i] == 0 ? 1u : 0u));   // only 0xFFFFFFFF skipped
    }
}

static void testStoppedResult(void)
{
    struct CaptureConfig config = ringConfig(16, 8);
    CHECK(startCapture(&config));
    struct R5CapReader reader;
    CHECK(R5CAPREADER_open(&reader, region, sizeof(region)));
    runBlocks(2);
    CAPTURE_stop();
    uint16_t samples[MAX_BLOCK_SAMPLES];
    struct R5CapBlockInfo info;
    // Blocks published before the stop are still delivered, then STOPPED
    CHECK(R5CAPREADER_read(&reader, samples, &info) == R5CAP_READ_OK);
    CHECK(R5CAPREADER_read(&reader, samples, &info) == R5CAP_READ_OK);
    CHECK(R5CAPREADER_read(&reader, samples, &info) == R5CAP_READ_STOPPED);
}

// ---- T0.4: one writer thread, several reader threads, a tiny ring ----
//
// Contention can be raised with environment variables (defaults in brackets):
//   T04_READERS [3]   reader threads; more than the CPU count forces readers to be
//                     preempted mid-copy, which is when torn reads happen
//   T04_RING    [4]   ring slots (power of two, >= 2); 2 makes every slow read overlap a rewrite
//   T04_FRAMES  [16]  frames per block (<= MAX_BLOCK_SAMPLES / 2); bigger = longer copies
//   T04_BLOCKS  [200000]
// Example stress run: T04_READERS=12 T04_RING=2 T04_FRAMES=64 ./test_ring

#define MAX_CONCURRENT_READERS 32

static uint32_t envOr(const char *name, uint32_t fallback, uint32_t min, uint32_t max)
{
    const char *text = getenv(name);
    if (text == NULL) {
        return fallback;
    }
    unsigned long value = strtoul(text, NULL, 10);
    return (value < min) ? min : (value > max) ? max : (uint32_t)value;
}

static uint32_t concurrentBlocks = 0;

struct ReaderResult {
    struct R5CapReader reader;
    uint64_t blocksOk;
    uint64_t blocksSkipped;
    uint64_t corrupt;        // returned blocks whose samples don't match: torn reads
    uint64_t outOfOrder;     // block numbers that don't follow previous + 1 + skipped
};

static void *writerThread(void *arg)
{
    (void)arg;
    runBlocks(concurrentBlocks);
    CAPTURE_stop();
    return NULL;
}

static void *readerThread(void *arg)
{
    struct ReaderResult *result = arg;
    uint16_t samples[MAX_BLOCK_SAMPLES];
    struct R5CapBlockInfo info;
    bool hasPrevious = false;
    uint32_t previous = 0;
    for (;;) {
        enum R5CapReadResult r = R5CAPREADER_read(&result->reader, samples, &info);
        if (r == R5CAP_READ_STOPPED) {
            break;
        }
        if (r != R5CAP_READ_OK) {
            continue;   // busy-poll: this test wants maximum contention
        }
        result->blocksOk++;
        result->blocksSkipped += info.skipped;
        if (!blockMatchesSignal(samples, &info)) {
            result->corrupt++;
        }
        if (hasPrevious && info.blockNumber != previous + 1 + info.skipped) {
            result->outOfOrder++;
        }
        hasPrevious = true;
        previous = info.blockNumber;
    }
    // Blocks lost right before the stop haven't been reported with a block yet
    result->blocksSkipped += result->reader.pendingSkipped;
    return NULL;
}

static void testConcurrentReaders(void)
{
    const uint32_t numReaders = envOr("T04_READERS", 3, 1, MAX_CONCURRENT_READERS);
    uint32_t ring = envOr("T04_RING", 4, 2, 1024);
    const uint32_t frames = envOr("T04_FRAMES", 16, 1, MAX_BLOCK_SAMPLES / 2);
    concurrentBlocks = envOr("T04_BLOCKS", 200000, 1, 100000000);
    while ((ring & (ring - 1)) != 0) {
        ring &= ring - 1;   // round down to a power of two
    }
    printf("  T0.4: %u readers, %u-slot ring, %u frames/block, %u blocks\n",
           numReaders, ring, frames, concurrentBlocks);

    struct CaptureConfig config = ringConfig(frames, ring);
    CHECK(startCapture(&config));

    struct ReaderResult results[MAX_CONCURRENT_READERS];
    memset(results, 0, sizeof(results));
    pthread_t readers[MAX_CONCURRENT_READERS];
    for (uint32_t i = 0; i < numReaders; i++) {
        CHECK(R5CAPREADER_open(&results[i].reader, region, sizeof(region)));
        pthread_create(&readers[i], NULL, readerThread, &results[i]);
    }
    pthread_t writer;
    pthread_create(&writer, NULL, writerThread, NULL);
    pthread_join(writer, NULL);
    for (uint32_t i = 0; i < numReaders; i++) {
        pthread_join(readers[i], NULL);
    }

    uint64_t totalCorrupt = 0;
    for (uint32_t i = 0; i < numReaders; i++) {
        const struct ReaderResult *r = &results[i];
        printf("  T0.4 reader %u: %llu blocks ok, %llu skipped, %llu corrupt, %llu out of order\n",
               i, (unsigned long long)r->blocksOk, (unsigned long long)r->blocksSkipped,
               (unsigned long long)r->corrupt, (unsigned long long)r->outOfOrder);
        totalCorrupt += r->corrupt;
        CHECK(r->corrupt == 0);
        CHECK(r->outOfOrder == 0);
        CHECK(r->blocksOk > 0);
        CHECK(r->blocksOk + r->blocksSkipped == concurrentBlocks);   // every block accounted for
    }
    printf("  T0.4 total corrupt: %llu\n", (unsigned long long)totalCorrupt);
}

int main(void)
{
    testOpenValidates();
    testReaderKeepsUp();
    testSlowReader();
    testSequenceWrap();
    testStoppedResult();
    testConcurrentReaders();
    return TEST_finish("test_ring");
}
