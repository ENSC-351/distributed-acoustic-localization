// Linux reserved-core sampler: see audio/CONTEXT.md

// For pinning the sampling thread to a CPU core
#define _GNU_SOURCE
#include "audio/audio_capture.h"
#include "hal/adc.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_BLOCK_FRAMES 128
#define DEFAULT_RING_BLOCKS 160
#define DEFAULT_CHANNEL_MIC0 0
#define DEFAULT_CHANNEL_MIC1 1
#define DEFAULT_NUM_CHANNELS 2
#define MIN_RING_BLOCKS 2

// Measuring the sample rate: time SPI messages of two sizes this many times
// each and keep the fastest (least disturbed) of each
#define CALIBRATION_ROUNDS 20
#define CALIBRATION_SMALL_DIVISOR 4

// A block whose measured frame period differs from the running average by
// more than this fraction is flagged as disturbed (for example, an interrupt
// paused the SPI transfer partway through)
#define DISTURBED_FRACTION 0.05

// The running average of the frame period follows changes over about this many blocks
#define PERIOD_AVERAGE_BLOCKS 32.0

// This many disturbed blocks in a row means the speed really changed (for
// example the CPU clock changed), so start the average again from there
#define RESYNC_BLOCKS 8

// After a failed SPI message, wait this long before retrying
#define ERROR_RETRY_NS (1 * NS_PER_MS)

// Cores reserved with the isolcpus= boot argument, such as "3" or "2-3" (empty if none)
#define ISOLATED_CPUS_PATH "/sys/devices/system/cpu/isolated"

#define NS_PER_S 1000000000LL
#define NS_PER_MS 1000000LL

struct AudioCapture_Reader {
    uint64_t nextBlock;       // Sequence number of the next block to return
    bool hasPrevious;
    int64_t previousEndNs;    // CLOCK_REALTIME just after the previous block's last frame
};

static AudioCapture_Config config;
static double framePeriodNs = 0.0;
static double callOverheadNs = 0.0;
static bool isRunning = false;
static pthread_t samplerThread;
static int reservedCpu = AUDIO_CAPTURE_NO_CPU;   // Set when capture starts
static atomic_bool stopRequested;

// Ring of blocks. Block number s is stored in slot s % ringBlocks. The sampling
// thread fills slot blocksWritten % ringBlocks outside the lock and then
// publishes it by incrementing blocksWritten. Readers may only copy blocks
// blocksWritten - (ringBlocks - 1) to blocksWritten - 1, and only while
// holding ringMutex, so they never touch the slot being filled.
static pthread_mutex_t ringMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t blockReady;
static pthread_once_t condOnce = PTHREAD_ONCE_INIT;
static uint16_t *ringSamples = NULL;
static int64_t *ringFirstNs = NULL;
static double *ringPeriodNs = NULL;
static bool *ringDisturbed = NULL;
static double averagePeriodNs = 0.0;   // Running average of undisturbed blocks
static uint64_t blocksWritten = 0;
static bool isStopping = true;
static AudioCapture_Stats sharedStats;

static int64_t nowNs(clockid_t clock)
{
    struct timespec t;
    clock_gettime(clock, &t);
    return (int64_t)t.tv_sec * NS_PER_S + t.tv_nsec;
}

static struct timespec toTimespec(int64_t ns)
{
    struct timespec t = { (time_t)(ns / NS_PER_S), (long)(ns % NS_PER_S) };
    return t;
}

static void sleepNs(int64_t ns)
{
    struct timespec t = toTimespec(ns);
    nanosleep(&t, NULL);
}

static uint16_t *slotSamples(uint64_t block)
{
    size_t slot = (size_t)(block % (uint64_t)config.ringBlocks);
    return ringSamples + slot * (size_t)config.blockFrames * (size_t)config.numChannels;
}

static bool readBlock(int numFrames, uint16_t *values)
{
    return ADC_read_frames(config.channels, config.numChannels, numFrames, config.spiSpeedHz, values);
}

// Measures how long one frame takes (framePeriodNs) and the fixed cost of
// each SPI message (callOverheadNs), from the fastest of several messages of
// two different sizes: time = overhead + frames * period.
static bool calibrate(uint16_t *buffer)
{
    const int largeFrames = config.blockFrames;
    const int smallFrames = config.blockFrames / CALIBRATION_SMALL_DIVISOR;
    int64_t fastestSmall = INT64_MAX;
    int64_t fastestLarge = INT64_MAX;

    for (int i = 0; i < CALIBRATION_ROUNDS; i++) {
        int64_t start = nowNs(CLOCK_MONOTONIC);
        bool okSmall = readBlock(smallFrames, buffer);
        int64_t middle = nowNs(CLOCK_MONOTONIC);
        bool okLarge = readBlock(largeFrames, buffer);
        int64_t end = nowNs(CLOCK_MONOTONIC);
        if (!okSmall || !okLarge) {
            return false;
        }
        if (middle - start < fastestSmall) {
            fastestSmall = middle - start;
        }
        if (end - middle < fastestLarge) {
            fastestLarge = end - middle;
        }
    }

    framePeriodNs = (double)(fastestLarge - fastestSmall) / (largeFrames - smallFrames);
    callOverheadNs = (double)fastestLarge - framePeriodNs * largeFrames;
    if (framePeriodNs <= 0.0) {
        // Timing too noisy to separate the two; fall back to the plain average
        framePeriodNs = (double)fastestLarge / largeFrames;
        callOverheadNs = 0.0;
    }
    if (callOverheadNs < 0.0) {
        callOverheadNs = 0.0;
    }
    return true;
}

static void *samplerLoop(void *arg)
{
    (void)arg;

    AudioCapture_Stats local = { .callOverheadNs = callOverheadNs };
    uint16_t *slot = slotSamples(blocksWritten);
    bool hasPrevious = false;
    int64_t previousEndNs = 0;
    bool reportedAdcError = false;
    double average = framePeriodNs;
    int disturbedRun = 0;

    while (!atomic_load(&stopRequested)) {
        int64_t callStart = nowNs(CLOCK_MONOTONIC);
        bool ok = readBlock(config.blockFrames, slot);
        int64_t callEnd = nowNs(CLOCK_MONOTONIC);

        if (!ok) {
            if (!reportedAdcError) {
                fprintf(stderr, "AudioCapture: ADC read failed (further errors are only counted)\n");
                reportedAdcError = true;
            }
            local.adcErrors++;
            hasPrevious = false;
            pthread_mutex_lock(&ringMutex);
            sharedStats = local;
            pthread_mutex_unlock(&ringMutex);
            sleepNs(ERROR_RETRY_NS);
            continue;
        }

        // Measure this block's frame period from how long the call took.
        // Measuring every block also tracks any drift in the rate.
        double blockPeriod = ((double)(callEnd - callStart) - callOverheadNs) / config.blockFrames;
        if (blockPeriod <= 0.0) {
            blockPeriod = average;
        }
        bool disturbed = blockPeriod > average * (1.0 + DISTURBED_FRACTION) ||
                         blockPeriod < average * (1.0 - DISTURBED_FRACTION);
        disturbedRun = disturbed ? disturbedRun + 1 : 0;
        if (disturbedRun >= RESYNC_BLOCKS) {
            average = blockPeriod;
            disturbed = false;
            disturbedRun = 0;
        }
        if (disturbed) {
            local.blocksDisturbed++;
        } else {
            average += (blockPeriod - average) / PERIOD_AVERAGE_BLOCKS;
        }

        // Assume the call's fixed overhead was split evenly before and after the samples
        int64_t blockNs = (int64_t)(blockPeriod * config.blockFrames);
        int64_t firstSampleMono = callStart + (int64_t)(callOverheadNs / 2);
        if (hasPrevious && firstSampleMono - previousEndNs > local.maxGapNs) {
            local.maxGapNs = firstSampleMono - previousEndNs;
        }
        previousEndNs = firstSampleMono + blockNs;
        hasPrevious = true;

        // Convert to wall-clock time. The offset is re-measured every block
        // in case the clock was adjusted.
        int64_t realtimeOffset = nowNs(CLOCK_REALTIME) - nowNs(CLOCK_MONOTONIC);

        local.blocksCaptured++;
        local.framesCaptured += (uint64_t)config.blockFrames;

        pthread_mutex_lock(&ringMutex);
        size_t slotIndex = (size_t)(blocksWritten % (uint64_t)config.ringBlocks);
        ringFirstNs[slotIndex] = firstSampleMono + realtimeOffset;
        ringPeriodNs[slotIndex] = blockPeriod;
        ringDisturbed[slotIndex] = disturbed;
        averagePeriodNs = average;
        blocksWritten++;
        sharedStats = local;
        pthread_cond_broadcast(&blockReady);
        pthread_mutex_unlock(&ringMutex);

        slot = slotSamples(blocksWritten);
    }
    return NULL;
}

static void initCondition(void)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&blockReady, &attr);
    pthread_condattr_destroy(&attr);
}

static bool isValidConfig(const AudioCapture_Config *c)
{
    if (c->numChannels < 1 || c->numChannels > AUDIO_CAPTURE_MAX_CHANNELS ||
        c->ringBlocks < MIN_RING_BLOCKS || c->spiSpeedHz > ADC_MAX_SPI_SPEED_HZ) {
        return false;
    }
    if (c->blockFrames < AUDIO_CAPTURE_MIN_BLOCK_FRAMES ||
        c->blockFrames > ADC_MAX_CONVERSIONS_PER_READ / c->numChannels) {
        fprintf(stderr, "AudioCapture: blockFrames must be %d to %d for %d channels\n",
                AUDIO_CAPTURE_MIN_BLOCK_FRAMES, ADC_MAX_CONVERSIONS_PER_READ / c->numChannels,
                c->numChannels);
        return false;
    }
    for (int i = 0; i < c->numChannels; i++) {
        if (c->channels[i] >= ADC_NUM_CHANNELS) {
            return false;
        }
    }
    return true;
}

// Shrinks the block if spidev can't fit it in one SPI message, keeping about
// the same amount of buffered audio. Returns false if even the smallest
// block doesn't fit.
static bool fitBlockToSpidev(AudioCapture_Config *c)
{
    int maxFrames = ADC_getMaxConversionsPerRead() / c->numChannels;
    if (c->blockFrames <= maxFrames) {
        return true;
    }
    if (maxFrames < AUDIO_CAPTURE_MIN_BLOCK_FRAMES) {
        fprintf(stderr, "AudioCapture: spidev allows only %d frames per SPI message, need at least %d. "
                        "Raise spidev.bufsiz (see audio/README.md)\n", maxFrames, AUDIO_CAPTURE_MIN_BLOCK_FRAMES);
        return false;
    }

    fprintf(stderr, "AudioCapture: spidev allows only %d frames per SPI message, so blocks are %d frames "
                    "instead of %d. That means more gaps between blocks; raise spidev.bufsiz to 65536 "
                    "for full-size blocks (see audio/README.md)\n", maxFrames, maxFrames, c->blockFrames);
    long long bufferedFrames = (long long)c->blockFrames * c->ringBlocks;
    c->blockFrames = maxFrames;
    c->ringBlocks = (int)((bufferedFrames + maxFrames - 1) / maxFrames);
    return true;
}

// The first core reserved with isolcpus=, or AUDIO_CAPTURE_NO_CPU
static int findReservedCpu(void)
{
    int cpu = AUDIO_CAPTURE_NO_CPU;
    FILE *file = fopen(ISOLATED_CPUS_PATH, "r");
    if (file != NULL) {
        if (fscanf(file, "%d", &cpu) != 1 || cpu < 0 || cpu >= CPU_SETSIZE) {
            cpu = AUDIO_CAPTURE_NO_CPU;
        }
        fclose(file);
    }
    return cpu;
}

// Starts the sampling thread pinned to the reserved core. Nothing else is
// scheduled there, so it can poll SPI without resting. It uses normal priority
// on purpose: real-time priority would starve the kernel's own work on that
// core, which stalls the whole system.
static bool startSamplerThread(void)
{
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET((size_t)reservedCpu, &cpus);

    pthread_attr_t attr;
    struct sched_param param = { .sched_priority = 0 };
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    pthread_attr_setschedparam(&attr, &param);
    int err = pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);
    if (err == 0) {
        err = pthread_create(&samplerThread, &attr, samplerLoop, NULL);
    }
    pthread_attr_destroy(&attr);
    if (err != 0) {
        fprintf(stderr, "AudioCapture: unable to start sampling on reserved CPU %d: %s\n",
                reservedCpu, strerror(err));
    }
    return err == 0;
}

static void freeRing(void)
{
    free(ringSamples);
    free(ringFirstNs);
    free(ringPeriodNs);
    free(ringDisturbed);
    ringSamples = NULL;
    ringFirstNs = NULL;
    ringPeriodNs = NULL;
    ringDisturbed = NULL;
}

AudioCapture_Config AudioCapture_defaultConfig(void)
{
    AudioCapture_Config defaults = {
        .numChannels = DEFAULT_NUM_CHANNELS,
        .channels = { DEFAULT_CHANNEL_MIC0, DEFAULT_CHANNEL_MIC1 },
        .blockFrames = DEFAULT_BLOCK_FRAMES,
        .spiSpeedHz = ADC_DEFAULT_SPI_SPEED_HZ,
        .ringBlocks = DEFAULT_RING_BLOCKS,
    };
    return defaults;
}

bool AudioCapture_init(const AudioCapture_Config *userConfig)
{
    pthread_once(&condOnce, initCondition);

    if (isRunning) {
        fprintf(stderr, "AudioCapture: already initialized\n");
        return false;
    }

    AudioCapture_Config newConfig = userConfig ? *userConfig : AudioCapture_defaultConfig();
    if (!isValidConfig(&newConfig)) {
        fprintf(stderr, "AudioCapture: invalid configuration\n");
        return false;
    }
    if (newConfig.spiSpeedHz == 0) {
        newConfig.spiSpeedHz = ADC_DEFAULT_SPI_SPEED_HZ;
    }

    reservedCpu = findReservedCpu();
    if (reservedCpu == AUDIO_CAPTURE_NO_CPU) {
        fprintf(stderr, "AudioCapture: no reserved CPU core. Boot with isolcpus=3 (see overlays/README.md)\n");
        return false;
    }

    if (!ADC_init()) {
        return false;
    }
    if (!fitBlockToSpidev(&newConfig)) {
        ADC_cleanup();
        return false;
    }

    size_t samplesPerBlock = (size_t)newConfig.blockFrames * (size_t)newConfig.numChannels;
    size_t blocks = (size_t)newConfig.ringBlocks;

    pthread_mutex_lock(&ringMutex);
    config = newConfig;
    ringSamples = calloc(blocks * samplesPerBlock, sizeof(uint16_t));
    ringFirstNs = calloc(blocks, sizeof(int64_t));
    ringPeriodNs = calloc(blocks, sizeof(double));
    ringDisturbed = calloc(blocks, sizeof(bool));
    bool allocated = ringSamples && ringFirstNs && ringPeriodNs && ringDisturbed;
    if (!allocated) {
        freeRing();
    }
    blocksWritten = 0;
    memset(&sharedStats, 0, sizeof(sharedStats));
    isStopping = true;   // No readers until the thread is running
    pthread_mutex_unlock(&ringMutex);

    if (!allocated) {
        fprintf(stderr, "AudioCapture: out of memory\n");
        ADC_cleanup();
        return false;
    }

    // The ring isn't visible to readers yet, so use its first slot
    if (!calibrate(ringSamples)) {
        perror("AudioCapture: unable to read the ADC");
        pthread_mutex_lock(&ringMutex);
        freeRing();
        pthread_mutex_unlock(&ringMutex);
        ADC_cleanup();
        return false;
    }

    pthread_mutex_lock(&ringMutex);
    averagePeriodNs = framePeriodNs;
    isStopping = false;
    pthread_mutex_unlock(&ringMutex);

    atomic_store(&stopRequested, false);
    if (!startSamplerThread()) {
        pthread_mutex_lock(&ringMutex);
        isStopping = true;
        freeRing();
        pthread_mutex_unlock(&ringMutex);
        ADC_cleanup();
        return false;
    }
    isRunning = true;
    return true;
}

void AudioCapture_cleanup(void)
{
    if (!isRunning) {
        return;
    }

    // Wake any waiting readers first, so they stop touching the ring
    pthread_mutex_lock(&ringMutex);
    isStopping = true;
    pthread_cond_broadcast(&blockReady);
    pthread_mutex_unlock(&ringMutex);

    atomic_store(&stopRequested, true);
    pthread_join(samplerThread, NULL);
    ADC_cleanup();

    pthread_mutex_lock(&ringMutex);
    freeRing();
    pthread_mutex_unlock(&ringMutex);
    isRunning = false;
}

double AudioCapture_getFramePeriodNs(void)
{
    pthread_mutex_lock(&ringMutex);
    double value = averagePeriodNs;
    pthread_mutex_unlock(&ringMutex);
    return value;
}

double AudioCapture_getSampleRateHz(void)
{
    double period = AudioCapture_getFramePeriodNs();
    return period > 0.0 ? NS_PER_S / period : 0.0;
}

double AudioCapture_getChannelOffsetNs(int mic)
{
    if (mic < 0 || mic >= config.numChannels) {
        return 0.0;
    }
    return mic * AudioCapture_getFramePeriodNs() / config.numChannels;
}

int AudioCapture_getNumChannels(void)
{
    return config.numChannels;
}

int AudioCapture_getBlockFrames(void)
{
    return config.blockFrames;
}

int AudioCapture_getReservedCpu(void)
{
    return reservedCpu;
}

uint32_t AudioCapture_getSpiSpeedHz(void)
{
    return config.spiSpeedHz;
}

void AudioCapture_getStats(AudioCapture_Stats *stats)
{
    if (stats == NULL) {
        return;
    }
    pthread_mutex_lock(&ringMutex);
    *stats = sharedStats;
    pthread_mutex_unlock(&ringMutex);
}

AudioCapture_Reader *AudioCapture_openReader(void)
{
    AudioCapture_Reader *reader = malloc(sizeof(*reader));
    if (reader == NULL) {
        return NULL;
    }

    pthread_mutex_lock(&ringMutex);
    bool running = !isStopping;
    reader->nextBlock = blocksWritten;
    pthread_mutex_unlock(&ringMutex);

    if (!running) {
        free(reader);
        return NULL;
    }
    reader->hasPrevious = false;
    reader->previousEndNs = 0;
    return reader;
}

void AudioCapture_closeReader(AudioCapture_Reader *reader)
{
    free(reader);
}

AudioCapture_Result AudioCapture_readBlock(AudioCapture_Reader *reader, uint16_t *samples,
                                           AudioCapture_BlockInfo *info, int timeoutMs)
{
    if (reader == NULL || samples == NULL || info == NULL) {
        return AUDIO_CAPTURE_STOPPED;
    }

    struct timespec until = toTimespec(nowNs(CLOCK_MONOTONIC) + (int64_t)timeoutMs * NS_PER_MS);

    pthread_mutex_lock(&ringMutex);
    while (!isStopping && reader->nextBlock >= blocksWritten) {
        if (timeoutMs < 0) {
            pthread_cond_wait(&blockReady, &ringMutex);
        } else if (timeoutMs == 0 ||
                   pthread_cond_timedwait(&blockReady, &ringMutex, &until) == ETIMEDOUT) {
            pthread_mutex_unlock(&ringMutex);
            return AUDIO_CAPTURE_TIMEOUT;
        }
    }
    if (isStopping) {
        pthread_mutex_unlock(&ringMutex);
        return AUDIO_CAPTURE_STOPPED;
    }

    // If the reader fell behind, skip to the oldest block still kept
    uint64_t skipped = 0;
    uint64_t keptBlocks = (uint64_t)config.ringBlocks - 1;
    uint64_t oldest = (blocksWritten > keptBlocks) ? blocksWritten - keptBlocks : 0;
    if (reader->nextBlock < oldest) {
        skipped = oldest - reader->nextBlock;
        reader->nextBlock = oldest;
    }

    size_t slot = (size_t)(reader->nextBlock % (uint64_t)config.ringBlocks);
    size_t samplesPerBlock = (size_t)config.blockFrames * (size_t)config.numChannels;
    memcpy(samples, ringSamples + slot * samplesPerBlock, samplesPerBlock * sizeof(uint16_t));
    info->blockNumber = reader->nextBlock;
    info->tFirstNs = ringFirstNs[slot];
    info->framePeriodNs = ringPeriodNs[slot];
    info->timingDisturbed = ringDisturbed[slot];
    info->numFrames = config.blockFrames;
    info->numChannels = config.numChannels;
    pthread_mutex_unlock(&ringMutex);

    info->blocksSkipped = reader->hasPrevious ? skipped : 0;
    info->gapBeforeNs = reader->hasPrevious ? info->tFirstNs - reader->previousEndNs : 0;
    reader->previousEndNs = info->tFirstNs + (int64_t)(info->framePeriodNs * info->numFrames);
    reader->hasPrevious = true;
    reader->nextBlock++;
    return AUDIO_CAPTURE_OK;
}
