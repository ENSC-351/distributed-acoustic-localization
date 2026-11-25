// r5cap_ring_read.c
// Reads the R5's capture ring (zephyr_app firmware) through /dev/mem for N
// seconds and writes the same output as adc_record, so tools/analyze_recording.py
// handles both and the two sampling paths can be compared directly.
//
// Usage on the BeagleY-AI (root for /dev/mem; the redirections run as you):
//   sudo ./r5cap_ring_read 10 > r5.wav 2> r5.txt
// r5.wav: 2 channels, 16-bit raw ADC counts, sample rate = the R5's frame rate.
// r5.txt: setup, ring statistics, ch1_offset_frames (mic-to-mic skew in frames),
//         and one "block,..." line per block (times from the R5's own timer).
#include "r5cap_heartbeat.h"
#include "r5cap_reader.h"
#include "r5cap_shared.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define NS_PER_S 1000000000LL
#define POLL_US 1000
#define WAV_HEADER_BYTES 44
#define NUM_CHANNELS 2

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

struct BlockRecord {
    uint32_t number;
    uint64_t firstTick;
    uint32_t flags;
    uint32_t skipped;
};

int main(int argc, char *argv[])
{
    double seconds = (argc > 1) ? atof(argv[1]) : 10.0;
    if (seconds <= 0) {
        seconds = 1;
    }
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);   // O_SYNC: uncached mapping
    if (fd < 0) {
        perror("open /dev/mem (run with sudo)");
        return 2;
    }
    void *region = mmap(NULL, R5CAP_SHM_SIZE, PROT_READ, MAP_SHARED, fd, R5CAP_SHM_PHYS);
    if (region == MAP_FAILED) {
        perror("mmap the r5capture shared region");
        return 2;
    }
    struct R5CapReader reader;
    if (!R5CAPREADER_open(&reader, region, R5CAP_SHM_SIZE)) {
        fprintf(stderr, "no capture ring at 0x%08X (wrong magic/version): is the R5 firmware"
                        " running and capturing? (r5cap stats)\n", R5CAP_SHM_PHYS);
        return 1;
    }
    const struct r5cap_header *h = region;
    const uint32_t frames = h->blockFrames;
    const uint32_t channels = h->numChannels;
    const double tickHz = h->tickHz;
    const double periodTicks = h->framePeriodTicks;
    const double rateHz = tickHz / periodTicks;
    const uint32_t lateAtStart = h->lateTicks;
    const uint32_t adcAtStart = h->adcErrors;
    if (channels != NUM_CHANNELS) {
        fprintf(stderr, "expected 2 channels, ring has %u\n", (unsigned)channels);
        return 1;
    }

    size_t maxBlocks = (size_t)(seconds * rateHz / frames) + 8;
    uint16_t *samples = malloc(maxBlocks * frames * channels * sizeof(uint16_t));
    struct BlockRecord *blocks = malloc(maxBlocks * sizeof(struct BlockRecord));
    if (samples == NULL || blocks == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    size_t numBlocks = 0;
    uint64_t skippedTotal = 0, lateBlocks = 0, adcBlocks = 0;
    int stopped = 0;
    int64_t end = nowNs() + (int64_t)(seconds * NS_PER_S);
    while (nowNs() < end && numBlocks < maxBlocks) {
        struct R5CapBlockInfo info;
        enum R5CapReadResult r =
            R5CAPREADER_read(&reader, samples + numBlocks * frames * channels, &info);
        if (r == R5CAP_READ_NONE) {
            usleep(POLL_US);
            continue;
        }
        if (r == R5CAP_READ_STOPPED) {
            stopped = 1;
            break;
        }
        blocks[numBlocks] = (struct BlockRecord){info.blockNumber, info.firstTick, info.flags,
                                                 info.skipped};
        skippedTotal += info.skipped;
        lateBlocks += (info.flags & R5CAP_BLOCK_LATE_TICK) ? 1 : 0;
        adcBlocks += (info.flags & R5CAP_BLOCK_ADC_ERROR) ? 1 : 0;
        numBlocks++;
    }
    if (numBlocks == 0) {
        fprintf(stderr, "no blocks arrived in %.1f s (R5 stopped? r5cap stats)\n", seconds);
        return 1;
    }

    // WAV header
    size_t totalFrames = numBlocks * frames;
    uint32_t dataBytes = (uint32_t)(totalFrames * channels * sizeof(uint16_t));
    uint32_t rate = (uint32_t)(rateHz + 0.5);
    uint8_t header[WAV_HEADER_BYTES];
    memcpy(header, "RIFF", 4);
    putLe(header + 4, 36 + dataBytes, 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    putLe(header + 16, 16, 4);
    putLe(header + 20, 1, 2);
    putLe(header + 22, NUM_CHANNELS, 2);
    putLe(header + 24, rate, 4);
    putLe(header + 28, rate * NUM_CHANNELS * 2, 4);
    putLe(header + 32, NUM_CHANNELS * 2, 2);
    putLe(header + 34, 16, 2);
    memcpy(header + 36, "data", 4);
    putLe(header + 40, dataBytes, 4);
    fwrite(header, 1, sizeof(header), stdout);
    fwrite(samples, sizeof(uint16_t), totalFrames * channels, stdout);

    double nsPerTick = 1e9 / tickHz;
    double blockNs = frames * periodTicks * nsPerTick;
    fprintf(stderr, "r5cap_ring_read\n");
    fprintf(stderr, "transfer_mode=R5 MCU R5F, timer-paced frames (SPI: see r5cap stats)\n");
    fprintf(stderr, "frames_per_block=%u\nframes=%zu\nblocks=%zu\n", (unsigned)frames, totalFrames,
            numBlocks);
    fprintf(stderr, "tick_hz=%.0f\nframe_period_ticks=%.0f\n", tickHz, periodTicks);
    fprintf(stderr, "long_run_rate_hz=%.3f\ninblock_rate_hz_upper_bound=%.3f\n", rateHz, rateHz);
    fprintf(stderr, "median_block_ns=%.0f\n", blockNs);
    fprintf(stderr, "ch1_offset_ticks=%u\nch1_offset_frames=%.4f\n",
            (unsigned)h->channelOffsetTicks[1], h->channelOffsetTicks[1] / periodTicks);
    fprintf(stderr, "blocks_skipped_by_reader=%llu\n", (unsigned long long)skippedTotal);
    fprintf(stderr, "blocks_flagged_late=%llu\nblocks_flagged_adc_error=%llu\n",
            (unsigned long long)lateBlocks, (unsigned long long)adcBlocks);
    fprintf(stderr, "r5_late_ticks_during_read=%u\nadc_errors=%u\n",
            (unsigned)(h->lateTicks - lateAtStart), (unsigned)(h->adcErrors - adcAtStart));
    fprintf(stderr, "capture_stopped_during_read=%s\n", stopped ? "yes" : "no");
    fprintf(stderr, "realtime_priority=n/a (R5)\n");
    for (size_t i = 0; i < numBlocks; i++) {
        double t0 = (double)(blocks[i].firstTick - blocks[0].firstTick) * nsPerTick;
        fprintf(stderr, "block,%u,%zu,%.0f,%.0f\n", (unsigned)blocks[i].number, i * frames, t0,
                t0 + blockNs);
    }
    free(samples);
    free(blocks);
    munmap(region, R5CAP_SHM_SIZE);
    close(fd);
    return 0;
}
