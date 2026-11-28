// R5 backend of AudioCapture: see audio_capture_r5.h
//
// The R5 firmware does the sampling; this file only reads its shared-memory
// ring. Each AudioCapture reader wraps its own R5CapReader (the ring supports
// any number of independent readers), so there is no thread and no extra
// copy. Readers poll: a block arrives every blockFrames / rate (6.4 ms at
// 128 frames and 20 kHz).
//
// Times: blocks carry the R5 timer tick of their first frame. r5_clock.c
// converts ticks to CLOCK_REALTIME, from pairs (Linux reads MCU_TIMER0 itself,
// once a second) or, if the timer can't be read, from block arrival times.
#include "audio_capture_r5.h"
#include "r5_clock.h"

#include "r5cap_control.h"
#include "r5cap_heartbeat.h"
#include "r5cap_reader.h"
#include "r5cap_shared.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define NS_PER_S 1000000000LL
#define NS_PER_MS 1000000LL

// MCU_TIMER0: the R5's system timer, whose ticks stamp the blocks
#define R5_TIMER_PHYS 0x04800000u
#define R5_TIMER_MAP_SIZE 0x1000u
#define R5_TIMER_TCRR_OFFSET 0x3Cu

#define POLL_NS (1 * NS_PER_MS)          // sleep between polls while waiting for a block
#define PAIR_INTERVAL_NS NS_PER_S        // how often to re-pair the clocks
#define PAIR_INTERVAL_START_NS (100 * NS_PER_MS)   // ... during the first PAIR_FAST_COUNT pairs (rate not fitted yet)
#define PAIR_FAST_COUNT 30
#define PAIR_TRIES 5                     // keep the tightest of this many reads
#define TIMER_CHECK_NS (2 * NS_PER_MS)   // timer sanity check: it must advance over this
#define MAX_PAIR_TO_BLOCK_S 5.0          // timer and newest block must agree within this

struct R5Reader {
    struct R5CapReader ring;
    bool hasPrevious;
    uint32_t lastNumber;
    uint64_t blockNumber;        // 64-bit block count for AudioCapture_BlockInfo
    uint64_t previousEndTick;
};

static void *region = NULL;
static size_t regionSize = 0;
static bool ownsMapping = false;           // mapped here (unmap on cleanup) vs given by a test
static const volatile uint32_t *timerCounter = NULL;
static void *timerMapping = NULL;
static atomic_bool stopping;
static atomic_int readsInProgress;       // cleanup waits for these before unmapping
static bool running = false;
static uint32_t startSeq = 0;

static pthread_mutex_t clockMutex = PTHREAD_MUTEX_INITIALIZER;
static struct R5Clock clock_;
static uint64_t newestTick = 0;            // latest block first tick seen (for extending the timer)
static int64_t lastPairMono = 0;

static const struct r5cap_header *header(void)
{
    return (const struct r5cap_header *)region;
}

static int64_t nowNs(clockid_t which)
{
    struct timespec t;
    clock_gettime(which, &t);
    return (int64_t)t.tv_sec * NS_PER_S + t.tv_nsec;
}

static void sleepNs(int64_t ns)
{
    struct timespec t = {(time_t)(ns / NS_PER_S), (long)(ns % NS_PER_S)};
    nanosleep(&t, NULL);
}

static void *mapPhysical(uint32_t phys, size_t size, int *fdOut)
{
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);   // O_SYNC: uncached
    if (fd < 0) {
        return NULL;
    }
    void *p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, (off_t)phys);
    if (fdOut != NULL) {
        *fdOut = fd;
    } else {
        close(fd);
    }
    return (p == MAP_FAILED) ? NULL : p;
}

static bool ringIsRunning(const void *r, size_t size)
{
    struct R5CapReader probe;
    if (!R5CAPREADER_open(&probe, (void *)r, size)) {
        return false;
    }
    return ((const struct r5cap_header *)r)->state == R5CAP_STATE_RUNNING;
}

// First tick of the newest published block, or 0 if none
static uint64_t newestBlockTick(void)
{
    const struct r5cap_header *h = header();
    uint32_t seq = h->writeSeq;
    R5CAP_BARRIER();
    if (seq == startSeq && seq == 0) {
        return 0;
    }
    uint32_t wanted = seq - 1;
    struct r5cap_block_meta *meta = r5cap_meta(region, wanted & (h->ringBlocks - 1));
    uint32_t before = meta->seq;
    R5CAP_BARRIER();
    uint64_t tick = meta->firstTick;
    R5CAP_BARRIER();
    return (before == wanted && meta->seq == wanted) ? tick : 0;
}

// Reads the timer between two CLOCK_REALTIME reads; keeps the tightest
static bool readPair(uint32_t *low, int64_t *realtimeNs)
{
    int64_t bestWidth = INT64_MAX;
    for (int i = 0; i < PAIR_TRIES; i++) {
        int64_t before = nowNs(CLOCK_REALTIME);
        uint32_t t = *timerCounter;
        int64_t after = nowNs(CLOCK_REALTIME);
        if (after - before < bestWidth) {
            bestWidth = after - before;
            *low = t;
            *realtimeNs = before + (after - before) / 2;
        }
    }
    return bestWidth != INT64_MAX;
}

// With clockMutex held
static void pairNow(void)
{
    uint32_t low;
    int64_t rt;
    if (timerCounter != NULL && newestTick != 0 && readPair(&low, &rt)) {
        R5Clock_addPair(&clock_, R5Clock_extendTick(low, newestTick), rt);
    }
    lastPairMono = nowNs(CLOCK_MONOTONIC);
}

// The timer must count at about tickHz and agree with the ring's block ticks;
// otherwise it isn't the R5's clock (or can't be read), so use arrivals.
static bool timerLooksRight(void)
{
    if (timerCounter == NULL) {
        return false;
    }
    uint32_t a = *timerCounter;
    int64_t t0 = nowNs(CLOCK_MONOTONIC);
    sleepNs(TIMER_CHECK_NS);
    uint32_t b = *timerCounter;
    int64_t t1 = nowNs(CLOCK_MONOTONIC);
    double expected = (double)(t1 - t0) * 1e-9 * header()->tickHz;
    double counted = (double)(uint32_t)(b - a);
    if (counted < 0.5 * expected || counted > 2.0 * expected) {
        fprintf(stderr, "AudioCapture (R5): timer at 0x%08x counted %.0f ticks, expected ~%.0f; "
                        "using block arrival times\n", R5_TIMER_PHYS, counted, expected);
        return false;
    }
    uint64_t near = newestBlockTick();
    if (near != 0 && header()->state == R5CAP_STATE_RUNNING) {
        uint64_t now = R5Clock_extendTick(b, near);
        double ahead = (double)(int64_t)(now - near) / header()->tickHz;
        if (ahead < -0.1 || ahead > MAX_PAIR_TO_BLOCK_S) {
            fprintf(stderr, "AudioCapture (R5): timer is %.3f s from the newest block; "
                            "using block arrival times\n", ahead);
            return false;
        }
    }
    return true;
}

static bool start(void)
{
    if (!ringIsRunning(region, regionSize)) {
        fprintf(stderr, "AudioCapture (R5): no running capture in the R5 ring. Start it first "
                        "(r5capture/board/r5_start.sh)\n");
        return false;
    }
    const struct r5cap_header *h = header();
    startSeq = h->writeSeq;
    atomic_store(&stopping, false);

    pthread_mutex_lock(&clockMutex);
    R5Clock_init(&clock_, h->tickHz);
    newestTick = newestBlockTick();
    if (!timerLooksRight()) {
        timerCounter = NULL;
    }
    pairNow();
    pthread_mutex_unlock(&clockMutex);

    running = true;
    return true;
}

bool AudioCaptureR5_isSampling(void)
{
    void *r = mapPhysical(R5CAP_SHM_PHYS, R5CAP_SHM_SIZE, NULL);
    if (r == NULL) {
        return false;
    }
    bool ok = ringIsRunning(r, R5CAP_SHM_SIZE);
    munmap(r, R5CAP_SHM_SIZE);
    return ok;
}

bool AudioCaptureR5_init(void)
{
    if (running) {
        return false;
    }
    region = mapPhysical(R5CAP_SHM_PHYS, R5CAP_SHM_SIZE, NULL);
    if (region == NULL) {
        perror("AudioCapture (R5): unable to map the R5 ring through /dev/mem (run as root)");
        return false;
    }
    regionSize = R5CAP_SHM_SIZE;
    ownsMapping = true;
    timerMapping = mapPhysical(R5_TIMER_PHYS, R5_TIMER_MAP_SIZE, NULL);
    timerCounter = timerMapping ? (const volatile uint32_t *)((uint8_t *)timerMapping + R5_TIMER_TCRR_OFFSET)
                                : NULL;
    if (!start()) {
        AudioCaptureR5_cleanup();
        munmap(region, regionSize);
        region = NULL;
        if (timerMapping) {
            munmap(timerMapping, R5_TIMER_MAP_SIZE);
            timerMapping = NULL;
        }
        return false;
    }
    return true;
}

bool AudioCaptureR5_initWithRegion(void *testRegion, size_t testRegionSize, const volatile uint32_t *testTimer)
{
    if (running) {
        return false;
    }
    region = testRegion;
    regionSize = testRegionSize;
    ownsMapping = false;
    timerCounter = testTimer;
    return start();
}

void AudioCaptureR5_cleanup(void)
{
    atomic_store(&stopping, true);
    if (!running) {
        return;
    }
    // Readers notice `stopping` within one poll; wait until none is still
    // touching the ring before unmapping it
    while (atomic_load(&readsInProgress) > 0) {
        sleepNs(POLL_NS / 4);
    }
    running = false;
    if (ownsMapping) {
        munmap(region, regionSize);
        if (timerMapping) {
            munmap(timerMapping, R5_TIMER_MAP_SIZE);
        }
    }
    region = NULL;
    timerMapping = NULL;
    timerCounter = NULL;
}

double AudioCaptureR5_getFramePeriodNs(void)
{
    if (!running) {
        return 0.0;
    }
    pthread_mutex_lock(&clockMutex);
    double ns = R5Clock_ticksToNs(&clock_, header()->framePeriodTicks);
    pthread_mutex_unlock(&clockMutex);
    return ns;
}

double AudioCaptureR5_getChannelOffsetNs(int mic)
{
    if (!running || mic < 0 || mic >= (int)header()->numChannels) {
        return 0.0;
    }
    pthread_mutex_lock(&clockMutex);
    double ns = R5Clock_ticksToNs(&clock_, header()->channelOffsetTicks[mic]);
    pthread_mutex_unlock(&clockMutex);
    return ns;
}

int AudioCaptureR5_getNumChannels(void)
{
    return running ? (int)header()->numChannels : 0;
}

int AudioCaptureR5_getBlockFrames(void)
{
    return running ? (int)header()->blockFrames : 0;
}

uint32_t AudioCaptureR5_getSpiSpeedHz(void)
{
    if (!running || !ownsMapping) {
        return 0;
    }
    const struct r5cap_control *ctl =
        (const struct r5cap_control *)((const uint8_t *)region + (R5CAP_CONTROL_PHYS - R5CAP_SHM_PHYS));
    return (ctl->magic == R5CAP_CONTROL_MAGIC) ? ctl->ackSpiHz : 0;
}

void AudioCaptureR5_getStats(AudioCapture_Stats *stats)
{
    if (stats == NULL) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    if (!running) {
        return;
    }
    const struct r5cap_header *h = header();
    uint32_t blocks = h->writeSeq - startSeq;
    stats->blocksCaptured = blocks;
    stats->framesCaptured = (uint64_t)blocks * h->blockFrames;
    stats->adcErrors = h->adcErrors;
    stats->blocksDisturbed = h->lateTicks;   // R5: late or missed frame ticks since its capture started
    stats->maxGapNs = 0;                     // the R5 samples without gaps between blocks
    stats->callOverheadNs = 0.0;
}

bool AudioCaptureR5_hasTimerClock(void)
{
    return running && timerCounter != NULL;
}

AudioCapture_Reader *AudioCaptureR5_openReader(void)
{
    if (!running) {
        return NULL;
    }
    struct R5Reader *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    if (!R5CAPREADER_open(&r->ring, region, regionSize)) {
        free(r);
        return NULL;
    }
    return (AudioCapture_Reader *)r;
}

void AudioCaptureR5_closeReader(AudioCapture_Reader *reader)
{
    free(reader);
}

// Updates the clock from this block and converts its first tick
static int64_t blockTimeNs(const struct R5CapBlockInfo *block, uint64_t endTick)
{
    pthread_mutex_lock(&clockMutex);
    if ((int64_t)(block->firstTick - newestTick) > 0) {
        newestTick = block->firstTick;
    }
    if (timerCounter != NULL) {
        int64_t interval = (clock_.count < PAIR_FAST_COUNT) ? PAIR_INTERVAL_START_NS : PAIR_INTERVAL_NS;
        if (nowNs(CLOCK_MONOTONIC) - lastPairMono >= interval || !R5Clock_isValid(&clock_)) {
            pairNow();
        }
    } else {
        R5Clock_addArrival(&clock_, endTick, nowNs(CLOCK_REALTIME));
    }
    int64_t t = R5Clock_toRealtimeNs(&clock_, block->firstTick);
    pthread_mutex_unlock(&clockMutex);
    return t;
}

static AudioCapture_Result readBlock(struct R5Reader *r, uint16_t *samples, AudioCapture_BlockInfo *info,
                                     int timeoutMs)
{
    int64_t deadline = nowNs(CLOCK_MONOTONIC) + (int64_t)timeoutMs * NS_PER_MS;
    struct R5CapBlockInfo block;
    for (;;) {
        if (atomic_load(&stopping)) {
            return AUDIO_CAPTURE_STOPPED;
        }
        enum R5CapReadResult result = R5CAPREADER_read(&r->ring, samples, &block);
        if (result == R5CAP_READ_OK) {
            break;
        }
        if (result == R5CAP_READ_STOPPED) {
            return AUDIO_CAPTURE_STOPPED;
        }
        if (timeoutMs == 0) {
            return AUDIO_CAPTURE_TIMEOUT;
        }
        int64_t wait = POLL_NS;
        if (timeoutMs > 0) {
            int64_t left = deadline - nowNs(CLOCK_MONOTONIC);
            if (left <= 0) {
                return AUDIO_CAPTURE_TIMEOUT;
            }
            if (left < wait) {
                wait = left;
            }
        }
        sleepNs(wait);
    }

    const struct r5cap_header *h = header();
    uint64_t blockTicks = (uint64_t)h->framePeriodTicks * h->blockFrames;
    uint64_t endTick = block.firstTick + blockTicks;

    if (r->hasPrevious) {
        r->blockNumber += (uint32_t)(block.blockNumber - r->lastNumber);
    } else {
        r->blockNumber = block.blockNumber;
    }
    r->lastNumber = block.blockNumber;

    info->blockNumber = r->blockNumber;
    info->tFirstNs = blockTimeNs(&block, endTick);
    pthread_mutex_lock(&clockMutex);
    info->framePeriodNs = R5Clock_ticksToNs(&clock_, h->framePeriodTicks);
    info->gapBeforeNs = r->hasPrevious
                            ? (int64_t)R5Clock_ticksToNs(&clock_, (double)(int64_t)(block.firstTick - r->previousEndTick))
                            : 0;
    pthread_mutex_unlock(&clockMutex);
    info->numFrames = (int)h->blockFrames;
    info->numChannels = (int)h->numChannels;
    info->blocksSkipped = r->hasPrevious ? block.skipped : 0;
    info->timingDisturbed = (block.flags & (R5CAP_BLOCK_LATE_TICK | R5CAP_BLOCK_ADC_ERROR)) != 0;

    r->previousEndTick = endTick;
    r->hasPrevious = true;
    return AUDIO_CAPTURE_OK;
}

AudioCapture_Result AudioCaptureR5_readBlock(AudioCapture_Reader *reader, uint16_t *samples,
                                             AudioCapture_BlockInfo *info, int timeoutMs)
{
    struct R5Reader *r = (struct R5Reader *)reader;
    if (r == NULL || samples == NULL || info == NULL) {
        return AUDIO_CAPTURE_STOPPED;
    }
    atomic_fetch_add(&readsInProgress, 1);
    AudioCapture_Result result = AUDIO_CAPTURE_STOPPED;
    if (running && !atomic_load(&stopping)) {
        result = readBlock(r, samples, info, timeoutMs);
    }
    atomic_fetch_sub(&readsInProgress, 1);
    return result;
}
