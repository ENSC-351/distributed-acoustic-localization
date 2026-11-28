// adc_record.c
// Records both microphone channels (MCP3208 ch0, ch1) as fast as the ADC allows,
// for offline analysis (tools/analyze_recording.py). Uses Joey's ADC driver
// (hal/adc.c) directly, so it works with either SPI setup: bit-banged spidev0
// (course overlay) or hardware MCU_SPI0 (overlays/), and needs no reserved core.
//
// Usage on the BeagleY-AI (root: /dev/spidev* is root-only). The redirections run
// as you, so the files can go to the NFS share:
//   sudo ./adc_record 10 > rec.wav 2> rec.txt
//   sudo ./adc_record 10 1000000 > rec.wav 2> rec.txt      (optional SPI clock in Hz;
//                                                           ignored by bit-banged SPI)
// rec.wav: 2 channels, 16-bit, raw ADC counts (0..4095), sample rate = long-run rate.
// rec.txt: setup, rates, and one "block,<index>,<first_frame>,<t_start_ns>,<t_end_ns>"
//          line per SPI message (for gap and timing analysis).
#include "hal/adc.h"
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NUM_CHANNELS 2
#define NS_PER_S 1000000000LL
#define MAX_RATE_GUESS 60000      // frames/s upper bound for sizing the buffer
#define RT_PRIORITY 50
#define WAV_HEADER_BYTES 44
#define BITS_PER_SAMPLE 16

struct BlockTime {
    int64_t startNs;
    int64_t endNs;
};

static int64_t nowNs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * NS_PER_S + t.tv_nsec;
}

static void putLe(uint8_t *p, uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; i++) {
        p[i] = (uint8_t)(value >> (8 * i));
    }
}

static int compareI64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: sudo %s <seconds> [spi_hz] > rec.wav 2> rec.txt\n", argv[0]);
        return 2;
    }
    double seconds = atof(argv[1]);
    uint32_t spiHz = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 0;
    if (seconds <= 0) {
        seconds = 1;
    }

    // Real-time priority keeps Linux from pausing us mid-block (falls back if not allowed)
    struct sched_param param = {.sched_priority = RT_PRIORITY};
    int rt = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;

    if (!ADC_init()) {
        fprintf(stderr, "ADC_init failed: is /dev/spidev0.0 there (or ADC_SPI_DEVICE set)?\n");
        return 1;
    }
    const int blockFrames = ADC_getMaxConversionsPerRead() / NUM_CHANNELS;
    const uint8_t channels[NUM_CHANNELS] = {0, 1};
    size_t maxFrames = (size_t)(seconds * MAX_RATE_GUESS) + (size_t)blockFrames;
    size_t maxBlocks = maxFrames / (size_t)blockFrames + 1;
    uint16_t *samples = malloc(maxFrames * NUM_CHANNELS * sizeof(uint16_t));
    struct BlockTime *blocks = malloc(maxBlocks * sizeof(struct BlockTime));
    if (samples == NULL || blocks == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    // Capture into memory only; nothing is written until the end
    size_t frames = 0;
    size_t numBlocks = 0;
    unsigned errors = 0;
    int64_t end = nowNs() + (int64_t)(seconds * NS_PER_S);
    while (nowNs() < end && frames + (size_t)blockFrames <= maxFrames) {
        int64_t t0 = nowNs();
        bool ok = ADC_read_frames(channels, NUM_CHANNELS, blockFrames, spiHz,
                                  samples + frames * NUM_CHANNELS);
        int64_t t1 = nowNs();
        if (!ok) {
            errors++;
            continue;
        }
        blocks[numBlocks].startNs = t0;
        blocks[numBlocks].endNs = t1;
        numBlocks++;
        frames += (size_t)blockFrames;
    }
    const char *mode = ADC_getTransferMode();
    ADC_cleanup();
    if (numBlocks == 0) {
        fprintf(stderr, "no samples captured (%u ADC errors)\n", errors);
        return 1;
    }

    // Rates: long-run (frames / elapsed) and in-block (median per-message period)
    double spanS = (double)(blocks[numBlocks - 1].endNs - blocks[0].startNs) / NS_PER_S;
    double longRunHz = (double)frames / spanS;
    int64_t *periods = malloc(numBlocks * sizeof(int64_t));
    for (size_t i = 0; i < numBlocks; i++) {
        periods[i] = blocks[i].endNs - blocks[i].startNs;
    }
    qsort(periods, numBlocks, sizeof(int64_t), compareI64);
    double medianBlockNs = (double)periods[numBlocks / 2];
    free(periods);

    // WAV: 16-bit, 2 channels, raw counts
    uint32_t dataBytes = (uint32_t)(frames * NUM_CHANNELS * sizeof(uint16_t));
    uint8_t header[WAV_HEADER_BYTES];
    uint32_t rate = (uint32_t)(longRunHz + 0.5);
    memcpy(header, "RIFF", 4);
    putLe(header + 4, 36 + dataBytes, 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    putLe(header + 16, 16, 4);
    putLe(header + 20, 1, 2);   // PCM
    putLe(header + 22, NUM_CHANNELS, 2);
    putLe(header + 24, rate, 4);
    putLe(header + 28, rate * NUM_CHANNELS * BITS_PER_SAMPLE / 8, 4);
    putLe(header + 32, NUM_CHANNELS * BITS_PER_SAMPLE / 8, 2);
    putLe(header + 34, BITS_PER_SAMPLE, 2);
    memcpy(header + 36, "data", 4);
    putLe(header + 40, dataBytes, 4);
    fwrite(header, 1, sizeof(header), stdout);
    fwrite(samples, sizeof(uint16_t), frames * NUM_CHANNELS, stdout);   // little-endian on ARM

    fprintf(stderr, "adc_record\n");
    fprintf(stderr, "transfer_mode=%s\n", mode);
    fprintf(stderr, "spi_hz_requested=%u\n", (unsigned)spiHz);
    fprintf(stderr, "realtime_priority=%s\n", rt ? "yes" : "no");
    fprintf(stderr, "channels=0,1\n");
    fprintf(stderr, "frames_per_block=%d\n", blockFrames);
    fprintf(stderr, "frames=%zu\nblocks=%zu\nadc_errors=%u\n", frames, numBlocks, errors);
    fprintf(stderr, "span_s=%.6f\n", spanS);
    fprintf(stderr, "long_run_rate_hz=%.1f\n", longRunHz);
    fprintf(stderr, "median_block_ns=%.0f\n", medianBlockNs);
    fprintf(stderr, "inblock_rate_hz_upper_bound=%.1f\n", blockFrames * 1e9 / medianBlockNs);
    for (size_t i = 0; i < numBlocks; i++) {
        fprintf(stderr, "block,%zu,%zu,%lld,%lld\n", i, i * (size_t)blockFrames,
                (long long)blocks[i].startNs, (long long)blocks[i].endNs);
    }
    free(samples);
    free(blocks);
    return 0;
}
