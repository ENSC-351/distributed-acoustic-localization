// Tests for the streaming detector (dsp/detector.h). Runs on the build host.
// Exit code is the number of failed checks.
//
// The main check compares the streaming detector, fed in blocks of many
// sizes, with a direct offline transcription of detector.py's _run() loop
// (reference_detect below): same triggers, same windows, same DC.
#include "dsp/detector.h"
#include "dsp/highpass.h"
#include "dsp/tdoa.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 20000.0
#define PERIOD_NS (1e9 / FS)

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

static unsigned long rng_state;
static double urand(void)
{
    rng_state = (rng_state * 1664525ul + 1013904223ul) & 0xFFFFFFFFul;
    return (double)(rng_state >> 8) / 16777216.0 * 2.0 - 1.0;
}

// ------------------------------------------------------------ test signals

struct record {
    size_t n;
    float *frames;   // interleaved a, b
};

static struct record make_quiet(size_t n, double dc_a, double dc_b, double noise, unsigned long seed)
{
    struct record r = {n, malloc(2 * n * sizeof(float))};
    rng_state = seed;
    for (size_t i = 0; i < n; i++) {
        r.frames[2 * i] = (float)(dc_a + noise * urand());
        r.frames[2 * i + 1] = (float)(dc_b + noise * urand());
    }
    return r;
}

// Decaying noise burst at `onset` in channel A, and `delay` frames later in B
static void add_burst(struct record *r, size_t onset, int delay, double amp, unsigned long seed)
{
    rng_state = seed;
    for (size_t k = 0; k < 2000; k++) {
        double v = amp * urand() * exp(-(double)k / 120.0);
        size_t ia = onset + k;
        size_t ib = (size_t)((long)onset + delay + (long)k);
        if (ia < r->n) {
            r->frames[2 * ia] += (float)v;
        }
        if (ib < r->n) {
            r->frames[2 * ib + 1] += (float)v;
        }
    }
}

// ------------------------------------------------------------ reference

struct ref_event {
    long trigger, start;
    double dc_a, dc_b;
    bool complete;
};

static int cmp_d(const void *x, const void *y)
{
    double a = *(const double *)x, b = *(const double *)y;
    return (a > b) - (a < b);
}

static double ref_median(const double *v, size_t n)
{
    double *t = malloc(n * sizeof(double));
    memcpy(t, v, n * sizeof(double));
    qsort(t, n, sizeof(double), cmp_d);
    double m = (n % 2) ? t[n / 2] : 0.5 * (t[n / 2 - 1] + t[n / 2]);
    free(t);
    return m;
}

// detector.py _run() with a noise model, written the obvious offline way
static size_t reference_detect(const struct record *r, const struct dsp_detector_config *cfg,
                               const struct dsp_detector_noise *noise, struct ref_event *out, size_t max)
{
    size_t n = r->n;
    long smooth = lround(cfg->smooth_ms * 1e-3 * FS);
    if (smooth < 1) {
        smooth = 1;
    }
    long pre = lround(cfg->pre_ms * 1e-3 * FS);
    long post = lround(cfg->post_ms * 1e-3 * FS);
    long holdoff = lround(cfg->holdoff_ms * 1e-3 * FS);
    double floor = noise->floor_energy > cfg->min_noise_floor ? noise->floor_energy : cfg->min_noise_floor;
    double thr = cfg->threshold_ratio * floor;

    double *e = calloc(n, sizeof(double));
    for (size_t i = 0; i < n; i++) {
        if ((long)i + 1 < smooth) {
            e[i] = 0.0;
            continue;
        }
        double s = 0.0;
        for (long k = 0; k < smooth; k++) {
            double da = r->frames[2 * (i - k)] - noise->dc_a;
            double db = r->frames[2 * (i - k) + 1] - noise->dc_b;
            s += da * da + db * db;
        }
        e[i] = s / smooth;
    }

    size_t count = 0;
    long pos = 0;
    while (pos < (long)n && count < max) {
        long trig = -1;
        for (long i = pos; i < (long)n; i++) {
            if (e[i] > thr) {
                trig = i;
                break;
            }
        }
        if (trig < 0) {
            break;
        }
        struct ref_event ev = {trig, trig - pre, NAN, NAN, false};
        if (trig - pre >= 0 && trig + post <= (long)n) {
            ev.complete = true;
            double *v = malloc((size_t)pre * sizeof(double));
            for (long k = 0; k < pre; k++) {
                v[k] = r->frames[2 * (trig - pre + k)];
            }
            ev.dc_a = ref_median(v, (size_t)pre);
            for (long k = 0; k < pre; k++) {
                v[k] = r->frames[2 * (trig - pre + k) + 1];
            }
            ev.dc_b = ref_median(v, (size_t)pre);
            free(v);
        }
        out[count++] = ev;
        long nxt = trig + holdoff;
        if (nxt >= (long)n) {
            break;
        }
        long quiet = -1;
        for (long i = nxt; i < (long)n; i++) {
            if (e[i] <= cfg->rearm_ratio * thr) {
                quiet = i;
                break;
            }
        }
        if (quiet < 0) {
            break;
        }
        pos = quiet;
    }
    free(e);
    return count;
}

// ------------------------------------------------------------ helpers

static struct dsp_detector_config base_config(void)
{
    struct dsp_detector_config cfg;
    dsp_detector_default_config(&cfg);
    cfg.sample_rate_hz = FS;
    return cfg;
}

// Pushes a record in blocks of block_frames; collects every window
struct got {
    enum dsp_detector_status status;
    uint32_t flags;
    long start, trigger;
    double dc_a, dc_b;
    int64_t t_first_ns;
};

static size_t run_stream(struct dsp_detector *det, const struct record *r, size_t block_frames,
                         int64_t t0_ns, struct got *out, size_t max)
{
    size_t count = 0;
    for (size_t f = 0; f < r->n; f += block_frames) {
        size_t nf = (r->n - f < block_frames) ? r->n - f : block_frames;
        struct dsp_detector_block blk = {t0_ns + (int64_t)llround((double)f * PERIOD_NS), PERIOD_NS, false, false};
        dsp_detector_push_float(det, r->frames + 2 * f, nf, &blk);
        struct dsp_detector_window w;
        while (dsp_detector_next(det, &w) && count < max) {
            struct got g = {w.status, w.flags, (long)w.start_frame, (long)(w.start_frame + w.trigger_index),
                            w.dc_a, w.dc_b, w.t_first_ns};
            out[count++] = g;
            if (w.status == DSP_DETECTOR_OK) {
                // The output is raw minus DC, on the same indices
                for (size_t i = 0; i < w.n; i += 37) {
                    CHECK(fabs(w.a[i] - (w.raw_a[i] - w.dc_a)) < 1e-3, "a != raw - dc at %zu", i);
                    CHECK(w.raw_a[i] == r->frames[2 * (w.start_frame + i)], "raw_a not the input at %zu", i);
                    CHECK(w.raw_b[i] == r->frames[2 * (w.start_frame + i) + 1], "raw_b not the input at %zu", i);
                }
            }
        }
    }
    return count;
}

// ------------------------------------------------------------ tests

static void test_config(void)
{
    struct dsp_detector_config cfg;
    dsp_detector_default_config(&cfg);
    CHECK(!dsp_detector_config_valid(&cfg), "sample rate 0 must be rejected");
    CHECK(dsp_detector_create(&cfg) == NULL, "create with fs = 0");
    cfg.sample_rate_hz = FS;
    CHECK(dsp_detector_config_valid(&cfg), "defaults + fs are valid");
    cfg.channel_b = 2;
    CHECK(!dsp_detector_config_valid(&cfg), "channel_b out of range");
    cfg = base_config();
    cfg.rearm_ratio = 1.5;
    CHECK(!dsp_detector_config_valid(&cfg), "rearm_ratio > 1");
    cfg = base_config();
    cfg.post_ms = 0;
    CHECK(!dsp_detector_config_valid(&cfg), "post_ms 0");
    cfg = base_config();
    cfg.threshold_ratio = NAN;
    CHECK(!dsp_detector_config_valid(&cfg), "NaN threshold");
    CHECK(!dsp_detector_config_valid(NULL), "NULL config");
}

// Streaming == offline reference, for many records and block sizes
static void test_matches_reference(void)
{
    const size_t blocks[] = {1, 7, 64, 128, 1000, 100000};
    struct dsp_detector_noise noise = {2048.0, 2048.0, 0.0};
    int records = 0;
    for (unsigned long seed = 1; seed <= 12; seed++) {
        struct record r = make_quiet(30000, 2048.0, 2048.0, 4.0, seed);
        // Bursts: well separated, close together (inside holdoff), one at
        // the very start (incomplete), one near the end (never completes)
        add_burst(&r, 50, 0, 800.0, seed * 11);
        add_burst(&r, 3000 + seed * 13, (int)(seed % 7) - 3, 600.0, seed * 17);
        add_burst(&r, 3000 + seed * 13 + 600, 2, 600.0, seed * 19);   // 30 ms later: within holdoff
        add_burst(&r, 12000, -5, 300.0 + 50.0 * (double)seed, seed * 23);
        add_burst(&r, 20000, 4, 900.0, seed * 29);
        add_burst(&r, 29900, 1, 900.0, seed * 31);
        // Noise floor: variance of uniform(-4, 4) is 16/3 per channel
        noise.floor_energy = 2.0 * 16.0 / 3.0;

        struct dsp_detector_config cfg = base_config();
        struct ref_event ref[32];
        size_t nref = reference_detect(&r, &cfg, &noise, ref, 32);
        CHECK(nref >= 4, "seed %lu: reference found only %zu events", seed, nref);

        for (size_t bi = 0; bi < sizeof(blocks) / sizeof(blocks[0]); bi++) {
            struct dsp_detector *det = dsp_detector_create(&cfg);
            dsp_detector_set_noise(det, &noise);
            struct got got[32];
            size_t ngot = run_stream(det, &r, blocks[bi], 1000000000LL, got, 32);
            // The reference also lists a last trigger whose window runs past
            // the end; the stream is still waiting for it
            size_t nref_emitted = nref;
            if (nref > 0 && !ref[nref - 1].complete && ref[nref - 1].start >= 0) {
                nref_emitted--;
            }
            CHECK(ngot == nref_emitted, "seed %lu block %zu: %zu windows, reference %zu", seed, blocks[bi],
                  ngot, nref_emitted);
            for (size_t k = 0; k < ngot && k < nref_emitted; k++) {
                CHECK(got[k].trigger == ref[k].trigger, "seed %lu block %zu ev %zu: trigger %ld vs %ld", seed,
                      blocks[bi], k, got[k].trigger, ref[k].trigger);
                bool ref_ok = ref[k].complete;
                CHECK((got[k].status == DSP_DETECTOR_OK) == ref_ok, "seed %lu ev %zu: status %d, ref complete %d",
                      seed, k, got[k].status, ref_ok);
                if (ref_ok && got[k].status == DSP_DETECTOR_OK) {
                    CHECK(got[k].start == ref[k].start, "start %ld vs %ld", got[k].start, ref[k].start);
                    CHECK(fabs(got[k].dc_a - ref[k].dc_a) < 1e-3, "dc_a %f vs %f", got[k].dc_a, ref[k].dc_a);
                    CHECK(fabs(got[k].dc_b - ref[k].dc_b) < 1e-3, "dc_b %f vs %f", got[k].dc_b, ref[k].dc_b);
                    int64_t want = 1000000000LL + (int64_t)llround((double)got[k].start * PERIOD_NS);
                    CHECK(llabs(got[k].t_first_ns - want) <= 1, "t_first %lld vs %lld",
                          (long long)got[k].t_first_ns, (long long)want);
                } else if (!ref_ok) {
                    CHECK(got[k].status == DSP_DETECTOR_INCOMPLETE, "start-of-stream window should be INCOMPLETE");
                }
            }
            CHECK(dsp_detector_dropped(det) == 0, "nothing should be dropped");
            dsp_detector_destroy(det);
        }
        free(r.frames);
        records++;
    }
    printf("  reference comparison: %d records x %zu block sizes\n", records, sizeof(blocks) / sizeof(blocks[0]));
}

// Calibration from the first second of a quiet stream
static void test_calibration(void)
{
    struct dsp_detector_config cfg = base_config();
    struct record r = make_quiet(40000, 2000.0, 2100.0, 4.0, 99);
    add_burst(&r, 10000, 0, 500.0, 5);   // during calibration: must not trigger
    add_burst(&r, 30000, 0, 500.0, 6);
    struct dsp_detector *det = dsp_detector_create(&cfg);
    CHECK(!dsp_detector_get_noise(det, NULL), "no noise model before calibration");
    struct got got[8];
    size_t n = run_stream(det, &r, 128, 0, got, 8);
    struct dsp_detector_noise noise;
    CHECK(dsp_detector_get_noise(det, &noise), "noise model after calibration");
    CHECK(fabs(noise.dc_a - 2000.0) < 0.5 && fabs(noise.dc_b - 2100.0) < 0.5, "dc %f %f", noise.dc_a,
          noise.dc_b);
    // Median energy of quiet uniform noise: below the mean 2*16/3, but the same order
    CHECK(noise.floor_energy > 2.0 && noise.floor_energy < 12.0, "floor %f", noise.floor_energy);
    CHECK(n == 1, "%zu windows (only the burst after calibration)", n);
    if (n == 1) {
        CHECK(got[0].status == DSP_DETECTOR_OK && labs(got[0].trigger - 30000) < 10, "trigger %ld",
              got[0].trigger);
    }
    dsp_detector_destroy(det);
    free(r.frames);
}

// Discontinuities and disturbed blocks
static void test_gaps(void)
{
    struct dsp_detector_config cfg = base_config();
    struct dsp_detector_noise noise = {2048.0, 2048.0, 2.0 * 16.0 / 3.0};
    struct record r = make_quiet(4096, 2048.0, 2048.0, 4.0, 3);
    add_burst(&r, 1000, 0, 800.0, 7);   // trigger near 1000; window 900..1300

    // 1. Discontinuity flagged inside the window's post-trigger part -> GAP
    struct dsp_detector *det = dsp_detector_create(&cfg);
    dsp_detector_set_noise(det, &noise);
    struct dsp_detector_block blk = {0, PERIOD_NS, false, false};
    dsp_detector_push_float(det, r.frames, 1100, &blk);
    blk.discontinuity = true;
    dsp_detector_push_float(det, r.frames + 2 * 1100, 2996, &blk);
    struct dsp_detector_window w;
    bool got = dsp_detector_next(det, &w);
    CHECK(got && w.status == DSP_DETECTOR_GAP, "gap after trigger: got %d status %d", got, got ? (int)w.status : -1);
    CHECK(got && w.n == 0 && w.a == NULL, "invalid window has no samples");
    dsp_detector_destroy(det);

    // 2. Discontinuity just before the trigger (less than pre_ms of history) -> GAP
    det = dsp_detector_create(&cfg);
    dsp_detector_set_noise(det, &noise);
    blk.discontinuity = false;
    dsp_detector_push_float(det, r.frames, 950, &blk);
    blk.discontinuity = true;
    dsp_detector_push_float(det, r.frames + 2 * 950, 3146, &blk);
    got = dsp_detector_next(det, &w);
    CHECK(got && w.status == DSP_DETECTOR_GAP, "gap before trigger: status %d", got ? (int)w.status : -1);
    dsp_detector_destroy(det);

    // 3. Disturbed block in the middle of the window -> GAP; frames still counted
    det = dsp_detector_create(&cfg);
    dsp_detector_set_noise(det, &noise);
    struct dsp_detector_block ok = {0, PERIOD_NS, false, false};
    struct dsp_detector_block bad = {0, PERIOD_NS, false, true};
    dsp_detector_push_float(det, r.frames, 1024, &ok);
    dsp_detector_push_float(det, r.frames + 2 * 1024, 128, &bad);
    dsp_detector_push_float(det, r.frames + 2 * 1152, 2944, &ok);
    got = dsp_detector_next(det, &w);
    CHECK(got && w.status == DSP_DETECTOR_GAP, "disturbed block: status %d", got ? (int)w.status : -1);
    CHECK(!dsp_detector_next(det, &w), "nothing else");
    dsp_detector_destroy(det);

    // 4. The same record with no gap -> one OK window
    det = dsp_detector_create(&cfg);
    dsp_detector_set_noise(det, &noise);
    blk.discontinuity = false;
    dsp_detector_push_float(det, r.frames, 4096, &blk);
    got = dsp_detector_next(det, &w);
    CHECK(got && w.status == DSP_DETECTOR_OK && w.n == 400, "no gap: status %d n %zu", got ? (int)w.status : -1,
          got ? w.n : 0);
    dsp_detector_destroy(det);
    free(r.frames);
}

static void test_clipping_and_uint16(void)
{
    struct dsp_detector_config cfg = base_config();
    struct dsp_detector_noise noise = {2048.0, 2048.0, 2.0 * 16.0 / 3.0};
    struct record r = make_quiet(4096, 2048.0, 2048.0, 4.0, 8);
    add_burst(&r, 1000, 0, 1500.0, 9);
    r.frames[2 * 1010] = 4095.0f;   // channel A hits the top
    uint16_t *u = malloc(2 * r.n * sizeof(uint16_t));
    for (size_t i = 0; i < 2 * r.n; i++) {
        double v = round(r.frames[i]);
        u[i] = (uint16_t)(v < 0 ? 0 : (v > 4095 ? 4095 : v));
        r.frames[i] = (float)u[i];   // the float run sees the same integers
    }
    struct dsp_detector_block blk = {0, PERIOD_NS, false, false};

    struct dsp_detector *d1 = dsp_detector_create(&cfg);
    struct dsp_detector *d2 = dsp_detector_create(&cfg);
    dsp_detector_set_noise(d1, &noise);
    dsp_detector_set_noise(d2, &noise);
    dsp_detector_push(d1, u, r.n, &blk);
    dsp_detector_push_float(d2, r.frames, r.n, &blk);
    struct dsp_detector_window w1, w2;
    bool g1 = dsp_detector_next(d1, &w1);
    bool g2 = dsp_detector_next(d2, &w2);
    CHECK(g1 && g2, "both found the burst");
    if (g1 && g2) {
        CHECK(w1.start_frame == w2.start_frame && w1.dc_a == w2.dc_a, "uint16 and float input agree");
        CHECK(w1.flags & DSP_DETECTOR_FLAG_CLIPPED_A, "clipping on A flagged (flags %x)", w1.flags);
        struct dsp_tdoa_input in = dsp_detector_tdoa_input(&w1);
        CHECK(in.clipped && in.n == w1.n && in.a == w1.a, "tdoa input built from the window");
    }
    dsp_detector_destroy(d1);
    dsp_detector_destroy(d2);
    free(u);
    free(r.frames);
}

// Windows nobody takes: the oldest are dropped and counted
static void test_drop_oldest(void)
{
    struct dsp_detector_config cfg = base_config();
    struct dsp_detector_noise noise = {2048.0, 2048.0, 2.0 * 16.0 / 3.0};
    struct record r = make_quiet(60000, 2048.0, 2048.0, 4.0, 10);
    for (int k = 0; k < 12; k++) {
        add_burst(&r, 1000 + (size_t)k * 4000, 0, 800.0, 40 + (unsigned long)k);
    }
    struct dsp_detector *det = dsp_detector_create(&cfg);
    dsp_detector_set_noise(det, &noise);
    struct dsp_detector_block blk = {0, PERIOD_NS, false, false};
    size_t waiting = dsp_detector_push_float(det, r.frames, r.n, &blk);
    CHECK(waiting == 8, "queue holds 8, has %zu", waiting);
    CHECK(dsp_detector_dropped(det) == 4, "dropped %u", dsp_detector_dropped(det));
    struct dsp_detector_window w;
    CHECK(dsp_detector_next(det, &w) && w.start_frame > 4000 * 4, "oldest kept is the 5th (start %llu)",
          (unsigned long long)w.start_frame);
    dsp_detector_destroy(det);
    free(r.frames);
}

// Detector -> estimator, end to end on a synthetic delayed burst
static void test_into_estimator(void)
{
    const double spacing = 0.20, c = 343.0;
    struct dsp_tdoa_config tcfg;
    dsp_tdoa_default_config(&tcfg);
    tcfg.sample_rate_hz = FS;
    tcfg.mic_spacing_m = spacing;
    struct dsp_detector_config cfg = base_config();
    struct dsp_detector_noise noise = {2048.0, 2048.0, 2.0 * 16.0 / 3.0};

    const int delays[] = {-10, -4, 0, 3, 9};
    for (size_t k = 0; k < sizeof(delays) / sizeof(delays[0]); k++) {
        struct record r = make_quiet(8192, 2048.0, 2048.0, 4.0, 50 + k);
        add_burst(&r, 3000, delays[k], 700.0, 60 + k);
        struct dsp_detector *det = dsp_detector_create(&cfg);
        dsp_detector_set_noise(det, &noise);
        struct dsp_tdoa_estimator *est = dsp_tdoa_create(&tcfg, 400);
        struct dsp_detector_block blk = {0, PERIOD_NS, false, false};
        dsp_detector_push_float(det, r.frames, r.n, &blk);
        struct dsp_detector_window w;
        bool got = dsp_detector_next(det, &w);
        CHECK(got && w.status == DSP_DETECTOR_OK, "delay %d: window", delays[k]);
        if (got && w.status == DSP_DETECTOR_OK) {
            struct dsp_tdoa_input in = dsp_detector_tdoa_input(&w);
            struct dsp_tdoa_result res;
            dsp_tdoa_estimate(est, &in, &res);
            double want_deg = asin(-c * (delays[k] / FS) / spacing) * 180.0 / 3.14159265358979323846;
            CHECK(res.status == DSP_TDOA_OK, "delay %d: status %s", delays[k], dsp_tdoa_status_str(res.status));
            CHECK(fabs(res.angle_deg - want_deg) < 1.0, "delay %d: angle %.2f, want %.2f", delays[k], res.angle_deg,
                  want_deg);
        }
        dsp_tdoa_destroy(est);
        dsp_detector_destroy(det);
        free(r.frames);
    }
}

// A window is ready exactly when its last sample arrives, not later
static void test_emitted_on_last_sample(void)
{
    struct dsp_detector_config cfg = base_config();
    struct dsp_detector_noise noise = {2048.0, 2048.0, 2.0 * 16.0 / 3.0};
    struct record r = make_quiet(4096, 2048.0, 2048.0, 4.0, 12);
    add_burst(&r, 1000, 0, 800.0, 13);
    struct dsp_detector_block blk = {0, PERIOD_NS, false, false};

    // Find the trigger frame with one pass
    struct dsp_detector *det = dsp_detector_create(&cfg);
    dsp_detector_set_noise(det, &noise);
    dsp_detector_push_float(det, r.frames, r.n, &blk);
    struct dsp_detector_window w;
    bool got = dsp_detector_next(det, &w);
    CHECK(got && w.status == DSP_DETECTOR_OK, "burst found");
    size_t end = got ? (size_t)w.start_frame + w.n : 0;   // one past the last sample
    dsp_detector_destroy(det);

    // Push up to one frame before the end: nothing; then the last frame: the window
    det = dsp_detector_create(&cfg);
    dsp_detector_set_noise(det, &noise);
    CHECK(dsp_detector_push_float(det, r.frames, end - 1, &blk) == 0, "not ready one frame early");
    blk.t_first_ns = (int64_t)llround((double)(end - 1) * PERIOD_NS);
    CHECK(dsp_detector_push_float(det, r.frames + 2 * (end - 1), 1, &blk) == 1, "ready on its last sample");
    dsp_detector_destroy(det);
    free(r.frames);
}

// High-pass: removes DC and hum, keeps clap frequencies
static double sine_gain(double freq_hz, double cutoff_hz)
{
    struct dsp_highpass hp;
    dsp_highpass_init(&hp, cutoff_hz, FS, 2);
    enum { N = 20000 };
    static uint16_t buf[2 * N];
    for (int i = 0; i < N; i++) {
        double v = 2048.0 + 500.0 * sin(2.0 * 3.14159265358979323846 * freq_hz * i / FS);
        buf[2 * i] = buf[2 * i + 1] = (uint16_t)lround(v);
    }
    dsp_highpass_process(&hp, buf, buf, N);
    double peak = 0.0;
    for (int i = N / 2; i < N; i++) {   // after it settles
        double d = fabs((double)buf[2 * i] - 2048.0);
        peak = d > peak ? d : peak;
    }
    return peak / 500.0;
}

static void test_highpass(void)
{
    struct dsp_highpass hp;
    CHECK(!dsp_highpass_init(&hp, 0.0, FS, 2) && !dsp_highpass_init(&hp, FS, FS, 2), "bad cut-offs rejected");
    CHECK(dsp_highpass_init(&hp, 500.0, FS, 2), "500 Hz at 20 kHz");
    uint16_t dc[2 * 64];
    for (int i = 0; i < 128; i++) {
        dc[i] = 1999;
    }
    dsp_highpass_process(&hp, dc, dc, 64);
    CHECK(dc[0] == 2048 && dc[126] == 2048, "DC removed without a step: %u %u", dc[0], dc[126]);
    double g50 = sine_gain(50.0, 500.0), g500 = sine_gain(500.0, 500.0), g3k = sine_gain(3000.0, 500.0);
    CHECK(g50 < 0.02, "50 Hz gain %.3f", g50);
    CHECK(fabs(g500 - 0.707) < 0.05, "gain at the cut-off %.3f", g500);
    CHECK(fabs(g3k - 1.0) < 0.05, "3 kHz gain %.3f", g3k);
}

int main(void)
{
    test_config();
    test_matches_reference();
    test_calibration();
    test_gaps();
    test_clipping_and_uint16();
    test_drop_oldest();
    test_emitted_on_last_sample();
    test_into_estimator();
    test_highpass();
    printf("test_detector: %d checks, %d failed\n", checks, failures);
    return failures;
}
