// sampler_stats.c
// Measures the CURRENT Linux sampling path (Joey's AudioCapture over spidev)
// on the BeagleY-AI: sample rate, gaps between blocks, time not sampled,
// dropped and disturbed blocks. This is the baseline the R5 sampler must beat.
//
// Usage on the board (root, so the sampling thread gets real-time priority):
//   sudo ./sampler_stats [seconds] [reader_delay_us]
//     seconds          how long to measure (default 30)
//     reader_delay_us  extra work per block in the reader, to see when a slow
//                      consumer (like the DSP) starts losing blocks (default 0)
#include "audio/audio_capture.h"
#include "hal/adc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NS_PER_S 1000000000LL
#define NS_PER_US 1000.0
#define DEFAULT_SECONDS 30
#define READ_TIMEOUT_MS 1000

static int64_t nowNs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * NS_PER_S + t.tv_nsec;
}

static int compareI64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static void readLine(const char *path, char *out, size_t size)
{
    FILE *f = fopen(path, "r");
    snprintf(out, size, "unknown");
    if (f != NULL) {
        if (fgets(out, (int)size, f) == NULL) {
            snprintf(out, size, "unknown");
        }
        out[strcspn(out, "\n")] = '\0';
        fclose(f);
    }
}

int main(int argc, char *argv[])
{
    int seconds = (argc > 1) ? atoi(argv[1]) : DEFAULT_SECONDS;
    int readerDelayUs = (argc > 2) ? atoi(argv[2]) : 0;
    if (seconds < 1) {
        seconds = 1;
    }

    char governor[64];
    char bufsiz[32];
    readLine("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor", governor, sizeof(governor));
    readLine("/sys/module/spidev/parameters/bufsiz", bufsiz, sizeof(bufsiz));

    // Same block-size fitting as audio_meter --bench
    AudioCapture_Config config = AudioCapture_defaultConfig();
    if (!ADC_init()) {
        fprintf(stderr, "ADC_init failed (is /dev/spidev0.0 there, and are you root?)\n");
        return 1;
    }
    int maxFrames = ADC_getMaxConversionsPerRead() / config.numChannels;
    const char *mode = ADC_getTransferMode();
    ADC_cleanup();
    if (config.blockFrames > maxFrames) {
        config.ringBlocks = config.ringBlocks * config.blockFrames / maxFrames;
        config.blockFrames = maxFrames;
    }
    if (!AudioCapture_init(&config)) {
        fprintf(stderr, "AudioCapture_init failed\n");
        return 1;
    }

    const int channels = AudioCapture_getNumChannels();
    const int frames = AudioCapture_getBlockFrames();
    uint16_t *samples = malloc((size_t)frames * channels * sizeof(uint16_t));
    size_t maxBlocks = (size_t)(seconds + 2) * 100000 / (size_t)frames + 16;
    int64_t *gaps = malloc(maxBlocks * sizeof(int64_t));
    if (samples == NULL || gaps == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    AudioCapture_Reader *reader = AudioCapture_openReader();
    uint64_t blocksReceived = 0, blocksSkipped = 0, blocksDisturbed = 0, framesReceived = 0;
    size_t gapCount = 0;
    int64_t gapSumNs = 0;
    double periodSumNs = 0.0;
    int64_t firstSampleNs = 0, lastEndNs = 0;

    printf("Measuring the Linux sampler for %d s (reader delay %d us per block)...\n", seconds,
           readerDelayUs);
    int64_t start = nowNs();
    int64_t end = start + (int64_t)seconds * NS_PER_S;
    while (nowNs() < end) {
        AudioCapture_BlockInfo info;
        if (AudioCapture_readBlock(reader, samples, &info, READ_TIMEOUT_MS) != AUDIO_CAPTURE_OK) {
            continue;
        }
        if (blocksReceived == 0) {
            firstSampleNs = info.tFirstNs;
        } else if (gapCount < maxBlocks) {
            gaps[gapCount++] = info.gapBeforeNs;
            gapSumNs += info.gapBeforeNs;
        }
        blocksReceived++;
        blocksSkipped += info.blocksSkipped;
        blocksDisturbed += info.timingDisturbed ? 1 : 0;
        framesReceived += (uint64_t)info.numFrames;
        periodSumNs += info.framePeriodNs;
        lastEndNs = info.tFirstNs + (int64_t)(info.framePeriodNs * info.numFrames);
        if (readerDelayUs > 0) {
            usleep((useconds_t)readerDelayUs);
        }
    }
    AudioCapture_Stats stats;
    AudioCapture_getStats(&stats);
    double inBlockRate = AudioCapture_getSampleRateHz();
    double lagNs = AudioCapture_getChannelOffsetNs(1);
    AudioCapture_closeReader(reader);
    AudioCapture_cleanup();

    qsort(gaps, gapCount, sizeof(int64_t), compareI64);
    double spanNs = (double)(lastEndNs - firstSampleNs);
    double meanPeriodNs = blocksReceived ? periodSumNs / (double)blocksReceived : 0.0;
    double sampledNs = (double)framesReceived * meanPeriodNs;
    uint64_t blocksTotal = blocksReceived + blocksSkipped;
    int buckets[6] = {0};
    const int64_t edges[5] = {10000, 50000, 100000, 1000000, 10000000};   // 10us 50us 100us 1ms 10ms
    for (size_t i = 0; i < gapCount; i++) {
        int b = 0;
        while (b < 5 && gaps[i] >= edges[b]) {
            b++;
        }
        buckets[b]++;
    }
#define PCT(p) (gapCount ? gaps[(size_t)((double)(gapCount - 1) * (p))] / NS_PER_US : 0.0)

    printf("\n== Setup\n");
    printf("  CPU governor: %s   spidev.bufsiz: %s   SPI framing: %s\n", governor, bufsiz, mode);
    printf("  block: %d frames x %d mics (spidev allows %d frames per SPI message)\n", frames,
           channels, maxFrames);
    printf("\n== Sample rate (per mic)\n");
    printf("  inside a block:      %8.0f samples/s  (frame period %.2f us)\n", inBlockRate,
           meanPeriodNs / NS_PER_US);
    printf("  long-run, with gaps: %8.0f samples/s  (%llu frames in %.3f s)\n",
           spanNs > 0 ? (double)framesReceived * NS_PER_S / spanNs : 0.0,
           (unsigned long long)framesReceived, spanNs / NS_PER_S);
    printf("  mic 1 sampled %.1f us after mic 0 (multiplexer skew)\n", lagNs / NS_PER_US);
    printf("\n== Gaps between blocks (%zu gaps)\n", gapCount);
    printf("  mean %.1f us   median %.1f us   p99 %.1f us   p99.9 %.1f us   max %.1f us\n",
           gapCount ? (double)gapSumNs / (double)gapCount / NS_PER_US : 0.0, PCT(0.5), PCT(0.99),
           PCT(0.999), gapCount ? gaps[gapCount - 1] / NS_PER_US : 0.0);
    printf("  <10us %d   10-50us %d   50-100us %d   100us-1ms %d   1-10ms %d   >10ms %d\n",
           buckets[0], buckets[1], buckets[2], buckets[3], buckets[4], buckets[5]);
    printf("  time NOT sampled (in gaps): %.2f %% of the run\n",
           spanNs > 0 ? 100.0 * (1.0 - sampledNs / spanNs) : 0.0);
    printf("\n== Blocks\n");
    printf("  captured by the sampler: %llu   received by this reader: %llu\n",
           (unsigned long long)stats.blocksCaptured, (unsigned long long)blocksReceived);
    printf("  dropped (reader fell behind): %llu = %.3f %%\n", (unsigned long long)blocksSkipped,
           blocksTotal ? 100.0 * (double)blocksSkipped / (double)blocksTotal : 0.0);
    printf("  disturbed timing (>5%% off):  %llu = %.3f %%\n", (unsigned long long)blocksDisturbed,
           blocksReceived ? 100.0 * (double)blocksDisturbed / (double)blocksReceived : 0.0);
    printf("  ADC errors: %llu   max gap seen by the sampler: %.1f us   SPI call overhead: %.1f us\n",
           (unsigned long long)stats.adcErrors, stats.maxGapNs / NS_PER_US,
           stats.callOverheadNs / NS_PER_US);

    free(samples);
    free(gaps);
    return 0;
}
