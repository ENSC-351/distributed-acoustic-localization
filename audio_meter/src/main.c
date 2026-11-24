// Demo: live amplitude bars for both microphones, plus a sampling benchmark.
//
// Usage:
//   ./audio_meter                meter at the default SPI clock
//   ./audio_meter <spi_hz>       meter at another SPI clock (sets the sample rate)
//   ./audio_meter --bench        compare sample rate and reading quality at several SPI clocks
//   ./audio_meter --spi-diag     measure where the time goes in each SPI conversion
// Ctrl+C to quit. Run as root for access to SPI. Needs a reserved CPU core.

#include "audio/audio_capture.h"
#include "audio/audio_level.h"
#include "hal/adc.h"
#include "level_bar.h"
#include "spi_diag.h"
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BAR_WIDTH 40
#define DISPLAY_PERIOD_MS 33   // ~30 frames per second
#define LABEL_LENGTH 32
#define SUFFIX_LENGTH 64
#define FOOTER_LENGTH 160
#define DECIMAL 10

#define NS_PER_S 1000000000LL
#define NS_PER_MS 1000000L
#define NS_PER_US 1000.0
#define HZ_PER_MHZ 1000000.0
#define MS_PER_S 1000
#define READ_TIMEOUT_MS 1000

// Benchmark settings
#define BENCH_SECONDS_PER_SPEED 3
#define CPU_GOVERNOR_PATH "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"
#define GOVERNOR_LENGTH 64

static volatile sig_atomic_t keepRunning = 1;

static void handleSigint(int signum)
{
    (void)signum;
    keepRunning = 0;
}

static void sleepMs(long ms)
{
    struct timespec delay = { ms / MS_PER_S, (ms % MS_PER_S) * NS_PER_MS };
    nanosleep(&delay, NULL);
}

static int64_t monotonicNs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * NS_PER_S + t.tv_nsec;
}

static void formatStats(char *buffer, size_t size)
{
    AudioCapture_Stats stats;
    AudioCapture_getStats(&stats);
    snprintf(buffer, size, "%.0f Hz per mic | mic 1 lags %.1f us | blocks %" PRIu64
             " | disturbed %" PRIu64 " | max gap %.0f us | ADC errors %" PRIu64,
             AudioCapture_getSampleRateHz(), AudioCapture_getChannelOffsetNs(1) / NS_PER_US,
             stats.blocksCaptured, stats.blocksDisturbed, stats.maxGapNs / NS_PER_US, stats.adcErrors);
}

static int runMeter(uint32_t spiSpeedHz)
{
    AudioCapture_Config captureConfig = AudioCapture_defaultConfig();
    captureConfig.spiSpeedHz = spiSpeedHz;
    if (!AudioCapture_init(&captureConfig)) {
        fprintf(stderr, "Failed to start audio capture\n");
        return EXIT_FAILURE;
    }
    if (!AudioLevel_init(NULL)) {
        fprintf(stderr, "Failed to start audio level module\n");
        AudioCapture_cleanup();
        return EXIT_FAILURE;
    }

    int numMics = AudioLevel_getNumMics();
    printf("Linux reserved-core sampler (CPU %d)\n", AudioCapture_getReservedCpu());
    printf("Microphone amplitude meter on ADC channels");
    for (int mic = 0; mic < numMics; mic++) {
        printf(" %u", captureConfig.channels[mic]);
    }
    printf(". Press Ctrl+C to stop.\n\n");

    char labelText[AUDIO_CAPTURE_MAX_CHANNELS][LABEL_LENGTH];
    char suffixText[AUDIO_CAPTURE_MAX_CHANNELS][SUFFIX_LENGTH];
    const char *labels[AUDIO_CAPTURE_MAX_CHANNELS];
    const char *suffixes[AUDIO_CAPTURE_MAX_CHANNELS];
    double levels[AUDIO_CAPTURE_MAX_CHANNELS];
    for (int mic = 0; mic < numMics; mic++) {
        snprintf(labelText[mic], LABEL_LENGTH, "mic %d (ch %u)", mic, captureConfig.channels[mic]);
        labels[mic] = labelText[mic];
        suffixes[mic] = suffixText[mic];
    }

    while (keepRunning) {
        for (int mic = 0; mic < numMics; mic++) {
            levels[mic] = AudioLevel_getLevel(mic);
            snprintf(suffixText[mic], SUFFIX_LENGTH, "  amp: %4d  raw: %4d",
                     AudioLevel_getAmplitude(mic), AudioLevel_getLastSample(mic));
        }
        char footer[FOOTER_LENGTH];
        formatStats(footer, sizeof(footer));
        LevelBar_printMulti(numMics, levels, labels, suffixes, footer, BAR_WIDTH);
        sleepMs(DISPLAY_PERIOD_MS);
    }

    AudioLevel_cleanup();
    AudioCapture_cleanup();
    return EXIT_SUCCESS;
}

typedef struct {
    double sum[AUDIO_CAPTURE_MAX_CHANNELS];
    double sumSquares[AUDIO_CAPTURE_MAX_CHANNELS];
    uint64_t count;
    uint64_t blocksSkipped;
} SampleStats;

// Reads blocks for a few seconds and gathers per-mic mean and noise
static void collectSamples(SampleStats *s, int64_t *elapsedNs)
{
    memset(s, 0, sizeof(*s));
    AudioCapture_Reader *reader = AudioCapture_openReader();
    int numChannels = AudioCapture_getNumChannels();
    uint16_t *buffer = malloc((size_t)AudioCapture_getBlockFrames() * (size_t)numChannels * sizeof(uint16_t));
    int64_t start = monotonicNs();
    int64_t end = start + BENCH_SECONDS_PER_SPEED * NS_PER_S;

    while (reader && buffer && keepRunning && monotonicNs() < end) {
        AudioCapture_BlockInfo info;
        if (AudioCapture_readBlock(reader, buffer, &info, READ_TIMEOUT_MS) != AUDIO_CAPTURE_OK) {
            continue;
        }
        s->blocksSkipped += info.blocksSkipped;
        for (int f = 0; f < info.numFrames; f++) {
            for (int c = 0; c < numChannels; c++) {
                double v = buffer[f * numChannels + c];
                s->sum[c] += v;
                s->sumSquares[c] += v * v;
            }
        }
        s->count += (uint64_t)info.numFrames;
    }
    *elapsedNs = monotonicNs() - start;
    free(buffer);
    AudioCapture_closeReader(reader);
}

// The SPI driver's overhead runs at CPU speed, so a governor that changes the
// clock can change the sample rate
static void printGovernor(void)
{
    FILE *file = fopen(CPU_GOVERNOR_PATH, "r");
    char governor[GOVERNOR_LENGTH] = "unknown";
    if (file != NULL) {
        if (fgets(governor, sizeof(governor), file) == NULL) {
            strcpy(governor, "unknown");
        }
        governor[strcspn(governor, "\n")] = '\0';
        fclose(file);
    }
    printf("CPU frequency governor: %s%s\n", governor,
           strcmp(governor, "performance") == 0 ? "" :
           " (use 'performance' for a steady sample rate; see audio/README.md)");
}

// Captures at several SPI clocks and reports rate, gaps, and reading quality
static int runBenchmark(void)
{
    static const uint32_t speedsHz[] = { 1000000, 1250000, 1500000, 2000000 };
    const int numSpeeds = (int)(sizeof(speedsHz) / sizeof(speedsHz[0]));
    AudioCapture_Config config = AudioCapture_defaultConfig();

    printf("Capturing %d s at each SPI clock. Keep the room quiet: the noise column\n"
           "should stay about the same as the clock goes up. If the mean shifts or the\n"
           "noise grows, that clock is too fast for the ADC.\n\n", BENCH_SECONDS_PER_SPEED);

    // Fit the block to what spidev allows up front, so capture doesn't warn every time
    if (!ADC_init()) {
        return EXIT_FAILURE;
    }
    int maxFrames = ADC_getMaxConversionsPerRead() / config.numChannels;
    printf("SPI transfers: %s.\n", ADC_getTransferMode());
    printGovernor();
    ADC_cleanup();
    printf("spidev allows %d frames (%d conversions) per SPI message.\n",
           maxFrames, maxFrames * config.numChannels);
    if (config.blockFrames > maxFrames) {
        printf("Using %d-frame blocks instead of %d. Raise spidev.bufsiz to 65536 for full-size\n"
               "blocks and fewer gaps (see audio/README.md).\n",
               maxFrames, config.blockFrames);
        config.ringBlocks = config.ringBlocks * config.blockFrames / maxFrames;
        config.blockFrames = maxFrames;
    }
    printf("\n%7s %6s %9s %9s %8s %8s %8s %9s %15s %15s %8s\n", "SPI MHz", "block", "rate Hz", "overall",
           "frame us", "lag us", "gap us", "max gap", "mic 0 mean/sd", "mic 1 mean/sd", "disturbed");

    for (int i = 0; i < numSpeeds && keepRunning; i++) {
        config.spiSpeedHz = speedsHz[i];
        if (!AudioCapture_init(&config)) {
            printf("%7.2f  (could not start)\n", speedsHz[i] / HZ_PER_MHZ);
            continue;
        }
        SampleStats samples;
        int64_t elapsedNs;
        collectSamples(&samples, &elapsedNs);
        AudioCapture_Stats stats;
        AudioCapture_getStats(&stats);
        int reservedCpu = AudioCapture_getReservedCpu();
        double rate = AudioCapture_getSampleRateHz();
        double periodNs = AudioCapture_getFramePeriodNs();
        double lagNs = AudioCapture_getChannelOffsetNs(1);
        AudioCapture_cleanup();

        // Long-run frames per second, including the gaps between blocks
        double overall = elapsedNs > 0 ? (double)samples.count * NS_PER_S / (double)elapsedNs : 0.0;
        printf("%7.2f %6d %9.0f %9.0f %8.1f %8.1f %8.1f %9.1f",
               speedsHz[i] / HZ_PER_MHZ, config.blockFrames, rate, overall, periodNs / NS_PER_US, lagNs / NS_PER_US,
               stats.callOverheadNs / NS_PER_US, stats.maxGapNs / NS_PER_US);
        for (int c = 0; c < config.numChannels && c < 2; c++) {
            double mean = samples.count ? samples.sum[c] / (double)samples.count : 0.0;
            double variance = samples.count ? samples.sumSquares[c] / (double)samples.count - mean * mean : 0.0;
            printf("   %6.1f / %5.1f", mean, sqrt(variance > 0.0 ? variance : 0.0));
        }
        printf("  %5.1f%%", stats.blocksCaptured ? 100.0 * (double)stats.blocksDisturbed / (double)stats.blocksCaptured : 0.0);
        if (stats.adcErrors > 0 || samples.blocksSkipped > 0) {
            printf("  (%" PRIu64 " ADC errors, %" PRIu64 " blocks skipped)", stats.adcErrors, samples.blocksSkipped);
        }
        printf("  reserved CPU %d", reservedCpu);
        printf("\n");
    }

    printf("\nblock:    frames per block (one SPI message)\n"
           "rate Hz:  samples per second per mic inside a block\n"
           "overall:  long-run samples per second per mic, including gaps between blocks\n"
           "lag us:   how long after mic 0 mic 1 is sampled in each frame\n"
           "gap us:   typical gap between blocks (the cost of one SPI call); max gap is the worst seen\n"
           "disturbed: blocks whose timing was off by more than 5%% (samples may be unevenly spaced)\n");
    return EXIT_SUCCESS;
}

static void printUsage(const char *program)
{
    fprintf(stderr, "Usage: %s [spi_hz | --bench | --spi-diag]\n", program);
    fprintf(stderr, "  spi_hz   SPI clock, up to %d (default %d); a faster clock gives a higher sample rate\n",
            ADC_MAX_SPI_SPEED_HZ, ADC_DEFAULT_SPI_SPEED_HZ);
    fprintf(stderr, "  --bench  compare sample rate and reading quality at several SPI clocks\n");
    fprintf(stderr, "  --spi-diag  measure where the time goes in each SPI conversion\n");
}

int main(int argc, char *argv[])
{
    struct sigaction action = { .sa_handler = handleSigint };
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);

    if (argc > 2) {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2 && strcmp(argv[1], "--bench") == 0) {
        return runBenchmark();
    }
    if (argc == 2 && strcmp(argv[1], "--spi-diag") == 0) {
        return SpiDiag_run();
    }

    uint32_t spiSpeedHz = ADC_DEFAULT_SPI_SPEED_HZ;
    if (argc == 2) {
        char *end;
        long value = strtol(argv[1], &end, DECIMAL);
        if (*end != '\0' || value <= 0 || value > ADC_MAX_SPI_SPEED_HZ) {
            printUsage(argv[0]);
            return EXIT_FAILURE;
        }
        spiSpeedHz = (uint32_t)value;
    }
    return runMeter(spiSpeedHz);
}
