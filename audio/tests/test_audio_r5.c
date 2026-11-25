// Tests for the R5 backend of AudioCapture and its tick -> CLOCK_REALTIME
// conversion. Runs on the build host with a fake ring in ordinary memory
// (written with the same seqlock protocol as the firmware) and a fake timer.
// Exit code is the number of failed checks.
#include "audio/audio_capture.h"
#include "audio_capture_r5.h"
#include "r5_clock.h"
#include "r5cap_shared.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
static int checks;

#define CHECK(cond, ...)                                             \
    do {                                                             \
        checks++;                                                    \
        if (!(cond)) {                                               \
            failures++;                                              \
            printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                                     \
            printf("\n");                                            \
        }                                                            \
    } while (0)

#define TICK_HZ 25000000u
#define FRAMES 128u
#define CHANNELS 2u
#define RING 8u
#define PERIOD_TICKS 1250u   // 20 kHz

static int64_t now_ns(clockid_t c)
{
    struct timespec t;
    clock_gettime(c, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

// ------------------------------------------------------------ fake R5

static _Alignas(64) uint8_t region[64 * 1024];
static uint64_t nextTick;          // first tick of the next block the fake R5 publishes

static struct r5cap_header *hdr(void)
{
    return (struct r5cap_header *)region;
}

static void fake_start(uint64_t firstTick)
{
    memset(region, 0, sizeof(region));
    struct r5cap_header *h = hdr();
    h->magic = R5CAP_MAGIC;
    h->version = R5CAP_VERSION;
    h->numChannels = CHANNELS;
    h->blockFrames = FRAMES;
    h->ringBlocks = RING;
    h->tickHz = TICK_HZ;
    h->framePeriodTicks = PERIOD_TICKS;
    h->channels[0] = 0;
    h->channels[1] = 1;
    h->channelOffsetTicks[1] = 438;   // 17.52 us
    h->writeSeq = 0;
    h->state = R5CAP_STATE_RUNNING;
    nextTick = firstTick;
}

// Publishes one block; sample value = block number * 1000 + frame * 2 + channel
static void fake_publish(uint32_t flags)
{
    struct r5cap_header *h = hdr();
    uint32_t number = h->writeSeq;
    uint32_t slot = number & (RING - 1);
    struct r5cap_block_meta *meta = r5cap_meta(region, slot);
    meta->seq = R5CAP_SEQ_WRITING;
    R5CAP_BARRIER();
    uint16_t *s = r5cap_slotSamples(region, slot);
    for (uint32_t i = 0; i < FRAMES * CHANNELS; i++) {
        s[i] = (uint16_t)((number * 1000u + i) & 0xFFFF);
    }
    meta->flags = flags;
    meta->firstTick = nextTick;
    R5CAP_BARRIER();
    meta->seq = number;
    R5CAP_BARRIER();
    h->writeSeq = number + 1;
    nextTick += (uint64_t)FRAMES * PERIOD_TICKS;
}

// A timer that runs at TICK_HZ from CLOCK_MONOTONIC (rate * (1 + ppm))
static volatile uint32_t fakeTimer;
static atomic_bool timerRun;
static double timerPpm;
static int64_t timerBaseMono;
static uint64_t timerBaseTick;

static uint64_t fake_tick_now(void)
{
    double s = (double)(now_ns(CLOCK_MONOTONIC) - timerBaseMono) * 1e-9;
    return timerBaseTick + (uint64_t)(s * TICK_HZ * (1.0 + timerPpm * 1e-6));
}

static void *timer_thread(void *arg)
{
    (void)arg;
    while (atomic_load(&timerRun)) {
        fakeTimer = (uint32_t)fake_tick_now();   // spin: a sleeping thread leaves the counter stale
    }
    return NULL;
}

// ------------------------------------------------------------ R5Clock

static void test_clock(void)
{
    // Extending 32-bit values across wraps
    CHECK(R5Clock_extendTick(5, 0x1FFFFFFF0ull) == 0x200000005ull, "just after a wrap");
    CHECK(R5Clock_extendTick(0xFFFFFFF0u, 0x200000005ull) == 0x1FFFFFFF0ull, "just before a wrap");
    CHECK(R5Clock_extendTick(1234, 0x300000000ull + 1000) == 0x300000000ull + 1234, "same epoch");
    CHECK(R5Clock_extendTick(0xFFFFFF00u, 100) == 0xFFFFFF00ull, "first epoch never goes negative");

    // Timer 0.2 % fast: the fit must find it
    struct R5Clock c;
    R5Clock_init(&c, TICK_HZ);
    double fast = 1.002;
    int64_t rt0 = 1700000000LL * 1000000000LL;
    for (int s = 0; s <= 10; s++) {
        uint64_t tick = 1000000ull + (uint64_t)llround(s * TICK_HZ * fast);
        R5Clock_addPair(&c, tick, rt0 + (int64_t)s * 1000000000LL);
    }
    double want = 1e9 / (TICK_HZ * fast);
    CHECK(fabs(c.nsPerTick / want - 1.0) < 1e-9, "fitted %.9f ns/tick, want %.9f", c.nsPerTick, want);
    uint64_t later = 1000000ull + (uint64_t)llround(10.5 * TICK_HZ * fast);
    int64_t got = R5Clock_toRealtimeNs(&c, later);
    CHECK(llabs(got - (rt0 + 10500000000LL)) < 100, "time 0.5 s after the last pair: off by %lld ns",
          (long long)(got - (rt0 + 10500000000LL)));
    CHECK(c.steps == 0, "no steps");

    // CLOCK_REALTIME stepped by +1 s: refit from the new pair, rate kept
    uint64_t t11 = 1000000ull + (uint64_t)llround(11 * TICK_HZ * fast);
    R5Clock_addPair(&c, t11, rt0 + 12000000000LL);
    CHECK(c.steps == 1, "step detected");
    CHECK(llabs(R5Clock_toRealtimeNs(&c, t11) - (rt0 + 12000000000LL)) < 10, "follows the stepped clock");
    CHECK(fabs(c.nsPerTick / want - 1.0) < 1e-9, "rate kept across the step");

    // A rate 5 % off is not believed
    struct R5Clock d;
    R5Clock_init(&d, TICK_HZ);
    R5Clock_addPair(&d, 0, 0);
    R5Clock_addPair(&d, (uint64_t)(3.0 * TICK_HZ), (int64_t)(3.0 * 1.05e9));
    CHECK(fabs(d.nsPerTick - 40.0) < 1e-9, "implausible rate rejected (%.4f)", d.nsPerTick);

    // Arrivals: the earliest arrival (least delay) wins
    struct R5Clock a;
    R5Clock_init(&a, TICK_HZ);
    const int64_t base = 5000000000LL;
    const int64_t delays[] = {900000, 300000, 1200000, 250000, 700000};   // ns after each block's end
    for (int i = 0; i < 5; i++) {
        uint64_t end = (uint64_t)(i + 1) * 128 * PERIOD_TICKS;
        R5Clock_addArrival(&a, end, base + (int64_t)((double)end * 40.0) + delays[i]);
    }
    int64_t t = R5Clock_toRealtimeNs(&a, 0);
    CHECK(llabs(t - (base + 250000)) < 10, "arrival estimate %lld, want %lld", (long long)t,
          (long long)(base + 250000));
}

// ------------------------------------------------------------ backend

static void test_reading(void)
{
    fake_start(1000000);
    for (int i = 0; i < 3; i++) {
        fake_publish(0);   // already there before init: readers start after them
    }
    CHECK(AudioCaptureR5_initWithRegion(region, sizeof(region), NULL), "init on a running ring");
    CHECK(!AudioCaptureR5_hasTimerClock(), "no timer given: arrival clock");
    CHECK(AudioCaptureR5_getNumChannels() == 2 && AudioCaptureR5_getBlockFrames() == 128, "geometry from header");
    CHECK(fabs(AudioCaptureR5_getFramePeriodNs() - 50000.0) < 1e-6, "frame period %f",
          AudioCaptureR5_getFramePeriodNs());
    CHECK(fabs(AudioCaptureR5_getChannelOffsetNs(1) - 17520.0) < 1e-6, "mic 1 offset %f",
          AudioCaptureR5_getChannelOffsetNs(1));
    CHECK(AudioCaptureR5_getChannelOffsetNs(0) == 0.0 && AudioCaptureR5_getChannelOffsetNs(5) == 0.0,
          "mic 0 and out of range");

    AudioCapture_Reader *r = AudioCaptureR5_openReader();
    CHECK(r != NULL, "reader");
    uint16_t buf[FRAMES * CHANNELS];
    AudioCapture_BlockInfo info;
    CHECK(AudioCaptureR5_readBlock(r, buf, &info, 0) == AUDIO_CAPTURE_TIMEOUT, "nothing new yet: timeout 0");
    int64_t t0 = now_ns(CLOCK_MONOTONIC);
    CHECK(AudioCaptureR5_readBlock(r, buf, &info, 5) == AUDIO_CAPTURE_TIMEOUT, "timeout 5 ms");
    int64_t waited = now_ns(CLOCK_MONOTONIC) - t0;
    CHECK(waited >= 5000000 && waited < 50000000, "waited %lld ns", (long long)waited);

    fake_publish(0);
    fake_publish(R5CAP_BLOCK_LATE_TICK);
    CHECK(AudioCaptureR5_readBlock(r, buf, &info, 0) == AUDIO_CAPTURE_OK, "block 3");
    CHECK(info.blockNumber == 3 && info.numFrames == 128 && info.numChannels == 2, "block 3 info");
    CHECK(buf[0] == 3000 && buf[255] == 3255, "block 3 samples %u %u", buf[0], buf[255]);
    CHECK(!info.timingDisturbed && info.blocksSkipped == 0 && info.gapBeforeNs == 0, "block 3 clean");
    int64_t first = info.tFirstNs;
    CHECK(AudioCaptureR5_readBlock(r, buf, &info, 0) == AUDIO_CAPTURE_OK, "block 4");
    CHECK(info.timingDisturbed, "late tick -> disturbed");
    CHECK(info.gapBeforeNs == 0, "no gap between consecutive blocks");
    // Arrival clock: both blocks were waiting, so the first estimate was late;
    // times only have to move forward and stay upper bounds
    CHECK(info.tFirstNs > first && info.tFirstNs <= now_ns(CLOCK_REALTIME), "arrival times ordered: %lld",
          (long long)(info.tFirstNs - first));

    // Fall behind: 20 blocks published, ring keeps 7
    for (int i = 0; i < 20; i++) {
        fake_publish(0);
    }
    CHECK(AudioCaptureR5_readBlock(r, buf, &info, 0) == AUDIO_CAPTURE_OK, "after falling behind");
    CHECK(info.blocksSkipped == 13, "skipped %llu", (unsigned long long)info.blocksSkipped);
    CHECK(info.blockNumber == 18, "block number %llu", (unsigned long long)info.blockNumber);
    CHECK(llabs(info.gapBeforeNs - 13 * 6400000LL) < 1000, "gap %lld", (long long)info.gapBeforeNs);

    AudioCapture_Stats st;
    AudioCaptureR5_getStats(&st);
    CHECK(st.blocksCaptured == 22 && st.framesCaptured == 22 * 128, "stats %llu",
          (unsigned long long)st.blocksCaptured);

    // R5 stops: remaining blocks, then STOPPED
    hdr()->state = R5CAP_STATE_STOPPED;
    int ok = 0;
    while (AudioCaptureR5_readBlock(r, buf, &info, 0) == AUDIO_CAPTURE_OK) {
        ok++;
    }
    CHECK(ok == 6, "drained %d remaining blocks", ok);
    CHECK(AudioCaptureR5_readBlock(r, buf, &info, 10) == AUDIO_CAPTURE_STOPPED, "stopped");
    AudioCaptureR5_closeReader(r);
    AudioCaptureR5_cleanup();

    // Not running: init refuses
    CHECK(!AudioCaptureR5_initWithRegion(region, sizeof(region), NULL), "refuses a stopped ring");
    hdr()->magic = 0;
    hdr()->state = R5CAP_STATE_RUNNING;
    CHECK(!AudioCaptureR5_initWithRegion(region, sizeof(region), NULL), "refuses a bad magic");
}

// With a running timer: block times match CLOCK_REALTIME
static void test_timer_clock(double ppm)
{
    timerPpm = ppm;
    timerBaseMono = now_ns(CLOCK_MONOTONIC);
    timerBaseTick = 0xFFFFFFFFull - TICK_HZ / 2;   // the 32-bit counter wraps 0.5 s in
    fakeTimer = (uint32_t)timerBaseTick;
    atomic_store(&timerRun, true);
    pthread_t th;
    pthread_create(&th, NULL, timer_thread, NULL);

    fake_start(fake_tick_now());
    fake_publish(0);
    CHECK(AudioCaptureR5_initWithRegion(region, sizeof(region), &fakeTimer), "init with timer");
    CHECK(AudioCaptureR5_hasTimerClock(), "timer accepted (ppm %.0f)", ppm);
    AudioCapture_Reader *r = AudioCaptureR5_openReader();
    uint16_t buf[FRAMES * CHANNELS];
    AudioCapture_BlockInfo info;
    int64_t worst = 0;
    for (int i = 0; i < 25; i++) {   // ~2.5 s: crosses the wrap and a few re-pairings
        struct timespec t = {0, 100000000};
        nanosleep(&t, NULL);
        nextTick = fake_tick_now();
        int64_t realNow = now_ns(CLOCK_REALTIME);
        fake_publish(0);
        if (AudioCaptureR5_readBlock(r, buf, &info, 100) != AUDIO_CAPTURE_OK) {
            CHECK(0, "read %d", i);
            continue;
        }
        int64_t err = info.tFirstNs - realNow;
        if (llabs(err) > worst) {
            worst = llabs(err);
        }
    }
    // Limited by how fresh the fake counter is (a spinning thread, but on a VM): allow 2 ms
    CHECK(worst < 2000000, "ppm %.0f: worst block time error %lld ns", ppm, (long long)worst);
    printf("  timer clock, %+.0f ppm: worst block-time error %.1f us\n", ppm, (double)worst / 1000.0);
    AudioCaptureR5_closeReader(r);
    AudioCaptureR5_cleanup();
    atomic_store(&timerRun, false);
    pthread_join(th, NULL);
}

// A timer that doesn't move is rejected (arrival clock instead)
static void test_dead_timer(void)
{
    fake_start(5000000);
    fake_publish(0);
    fakeTimer = 1234;
    CHECK(AudioCaptureR5_initWithRegion(region, sizeof(region), &fakeTimer), "init with a dead timer");
    CHECK(!AudioCaptureR5_hasTimerClock(), "dead timer rejected");
    AudioCaptureR5_cleanup();
}

// cleanup() while another thread waits forever for a block: it returns STOPPED
static AudioCapture_Reader *waitingReader;
static atomic_int waitResult;

static void *wait_thread(void *arg)
{
    (void)arg;
    uint16_t buf[FRAMES * CHANNELS];
    AudioCapture_BlockInfo info;
    atomic_store(&waitResult, (int)AudioCaptureR5_readBlock(waitingReader, buf, &info, -1));
    return NULL;
}

static void test_cleanup_wakes_reader(void)
{
    fake_start(1000);
    fake_publish(0);
    CHECK(AudioCaptureR5_initWithRegion(region, sizeof(region), NULL), "init");
    waitingReader = AudioCaptureR5_openReader();
    atomic_store(&waitResult, -1);
    pthread_t th;
    pthread_create(&th, NULL, wait_thread, NULL);
    struct timespec t = {0, 20000000};
    nanosleep(&t, NULL);
    CHECK(atomic_load(&waitResult) == -1, "reader is waiting");
    AudioCaptureR5_cleanup();
    pthread_join(th, NULL);
    CHECK(atomic_load(&waitResult) == AUDIO_CAPTURE_STOPPED, "waiting reader got %d", atomic_load(&waitResult));
    AudioCaptureR5_closeReader(waitingReader);
}

int main(void)
{
    test_clock();
    test_reading();
    test_timer_clock(0.0);
    test_timer_clock(2000.0);   // 0.2 % fast, as hal_r5_timer.c's measurement hints
    test_dead_timer();
    test_cleanup_wakes_reader();
    printf("test_audio_r5: %d checks, %d failed\n", checks, failures);
    return failures;
}
