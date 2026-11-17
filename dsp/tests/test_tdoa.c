// Offline tests for the TDOA estimator, using synthetic windows with known
// delays. Runs on the build host. Exit code is the number of failed checks.
//
// Test geometry: fs = 40 kHz, d = 0.30 m, c = 343 m/s, so d/c is about
// 35.0 samples. These are placeholders until the real spacing and per-channel
// rate are measured.
#include "dsp/tdoa.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define FS      40000.0
#define SPACING 0.30
#define C_SOUND 343.0

#define N       1024           // window length per channel
#define MARGIN  128            // max |delay| the generator supports
#define SRC_LEN (N + 2 * MARGIN)
#define ONSET   (MARGIN + 300) // impact starts ~300 samples into the window

static int failures;
static int checks;

#define CHECK(cond, ...)                                           \
    do {                                                           \
        checks++;                                                  \
        if (!(cond)) {                                             \
            failures++;                                            \
            printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                                   \
            printf("\n");                                          \
        }                                                          \
    } while (0)

// Small deterministic generator so results are reproducible everywhere
static unsigned long rng_state;
static double urand(void)
{
    rng_state = (rng_state * 1664525ul + 1013904223ul) & 0xFFFFFFFFul;
    return (double)(rng_state >> 8) / 16777216.0 * 2.0 - 1.0;
}

// Decaying noise burst, a rough stand-in for an impact. smooth > 1 applies a
// moving average so the burst is band-limited like real microphone data.
static void make_source(double *src, unsigned long seed, int smooth)
{
    rng_state = seed;
    for (int i = 0; i < SRC_LEN; i++) {
        src[i] = (i < ONSET) ? 0.0 : urand() * exp(-(double)(i - ONSET) / 80.0);
    }
    for (int pass = 1; pass < smooth; pass++) {
        for (int i = SRC_LEN - 1; i > 0; i--) {
            src[i] = 0.5 * (src[i] + src[i - 1]);
        }
    }
}

// a[i] = src[i], b[i] = src[i - delay]: B hears the same thing `delay` samples later
static void make_pair(float *a, float *b, int delay, double noise, unsigned long seed, int smooth)
{
    static double src[SRC_LEN];
    make_source(src, seed, smooth);
    rng_state = seed ^ 0x5A5A5Aul;
    for (int i = 0; i < N; i++) {
        a[i] = (float)(src[i + MARGIN] + noise * urand());
        b[i] = (float)(src[i + MARGIN - delay] + noise * urand());
    }
}

static struct dsp_tdoa_config test_config(enum dsp_tdoa_method method)
{
    struct dsp_tdoa_config cfg;
    dsp_tdoa_default_config(&cfg);
    cfg.sample_rate_hz = FS;
    cfg.mic_spacing_m = SPACING;
    cfg.speed_of_sound_mps = C_SOUND;
    cfg.method = method;
    return cfg;
}

static double expected_angle(double tdoa)
{
    return asin(-C_SOUND * tdoa / SPACING) * 180.0 / 3.14159265358979323846;
}

static struct dsp_tdoa_result run(struct dsp_tdoa_estimator *est, const float *a, const float *b, size_t n)
{
    struct dsp_tdoa_input in = { .a = a, .b = b, .n = n, .clipped = false };
    struct dsp_tdoa_result res;
    dsp_tdoa_estimate(est, &in, &res);
    return res;
}

static const char *method_name(enum dsp_tdoa_method m)
{
    return m == DSP_TDOA_XCORR ? "xcorr" : "gcc-phat";
}

static float a[N], b[N];

// Positive, zero and negative delays; sign, magnitude and angle
static void test_known_delays(enum dsp_tdoa_method m)
{
    struct dsp_tdoa_config cfg = test_config(m);
    struct dsp_tdoa_estimator *est = dsp_tdoa_create(&cfg, N);
    const int delays[] = { -30, -12, -1, 0, 1, 7, 12, 30, 34 };

    for (size_t i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) {
        for (int smooth = 1; smooth <= 3; smooth += 2) {
            int d = delays[i];
            make_pair(a, b, d, 0.02, 1000u + (unsigned long)i, smooth);
            struct dsp_tdoa_result r = run(est, a, b, N);
            double want_tdoa = d / FS;
            CHECK(r.status == DSP_TDOA_OK, "%s delay %d smooth %d: %s",
                  method_name(m), d, smooth, dsp_tdoa_status_str(r.status));
            CHECK(r.peak_lag == d, "%s delay %d smooth %d: peak lag %d",
                  method_name(m), d, smooth, r.peak_lag);
            CHECK(fabs(r.raw_lag_samples - d) < 0.25, "%s delay %d: raw lag %.3f",
                  method_name(m), d, r.raw_lag_samples);
            CHECK(fabs(r.tdoa_s - want_tdoa) < 0.25 / FS, "%s delay %d: tdoa %g want %g",
                  method_name(m), d, r.tdoa_s, want_tdoa);
            CHECK(fabs(r.angle_deg - expected_angle(r.tdoa_s)) < 1e-9, "%s delay %d: angle %f",
                  method_name(m), d, r.angle_deg);
            // A first (d > 0) means the source is on A's side: negative angle
            CHECK(d == 0 ? fabs(r.angle_deg) < 0.5 : (d > 0) == (r.angle_deg < 0.0),
                  "%s delay %d: angle sign %f", method_name(m), d, r.angle_deg);
            CHECK(r.confidence >= 0.0 && r.confidence <= 1.0, "confidence %f", r.confidence);
        }
    }
    dsp_tdoa_destroy(est);
}

// Swapping the channels must negate the delay
static void test_channel_swap(enum dsp_tdoa_method m)
{
    struct dsp_tdoa_config cfg = test_config(m);
    struct dsp_tdoa_estimator *est = dsp_tdoa_create(&cfg, N);
    make_pair(a, b, 9, 0.02, 77u, 2);
    struct dsp_tdoa_result fwd = run(est, a, b, N);
    struct dsp_tdoa_result rev = run(est, b, a, N);
    CHECK(fwd.status == DSP_TDOA_OK && rev.status == DSP_TDOA_OK, "%s swap statuses %d %d",
          method_name(m), fwd.status, rev.status);
    CHECK(fwd.peak_lag == 9 && rev.peak_lag == -9, "%s swap lags %d %d",
          method_name(m), fwd.peak_lag, rev.peak_lag);
    CHECK(fabs(fwd.angle_deg + rev.angle_deg) < 0.5, "%s swap angles %f %f",
          method_name(m), fwd.angle_deg, rev.angle_deg);
    dsp_tdoa_destroy(est);
}

// MCP3208-style skew: B is sampled 3 samples after A, physical delay 10.
// The recorded data then only shows a 7-sample shift.
static void test_skew(enum dsp_tdoa_method m)
{
    struct dsp_tdoa_config cfg = test_config(m);
    cfg.channel_skew_s = 3.0 / FS;
    struct dsp_tdoa_estimator *est = dsp_tdoa_create(&cfg, N);
    make_pair(a, b, 10 - 3, 0.02, 31u, 2);
    struct dsp_tdoa_result r = run(est, a, b, N);
    CHECK(r.status == DSP_TDOA_OK, "%s skew: %s", method_name(m), dsp_tdoa_status_str(r.status));
    CHECK(r.peak_lag == 7, "%s skew: raw lag %d", method_name(m), r.peak_lag);
    CHECK(fabs(r.tdoa_s - 10.0 / FS) < 0.25 / FS, "%s skew: tdoa %g", method_name(m), r.tdoa_s);
    CHECK(fabs(r.skew_applied_s - 3.0 / FS) < 1e-12, "%s skew applied %g", method_name(m), r.skew_applied_s);
    // Search range shifts with skew: physical +-35.99 samples becomes -38..32
    CHECK(r.min_lag == -38 && r.max_lag == 32, "%s skew range %d..%d", method_name(m), r.min_lag, r.max_lag);
    dsp_tdoa_destroy(est);

    // Without the correction the same data gives the uncorrected delay
    cfg.channel_skew_s = 0.0;
    est = dsp_tdoa_create(&cfg, N);
    r = run(est, a, b, N);
    CHECK(fabs(r.tdoa_s - 7.0 / FS) < 0.25 / FS, "%s no-skew tdoa %g", method_name(m), r.tdoa_s);
    dsp_tdoa_destroy(est);
}

// d/c is 34.99 samples; tolerance is 1 sample
static void test_physical_limits(enum dsp_tdoa_method m)
{
    struct dsp_tdoa_config cfg = test_config(m);
    cfg.interpolate = false; // keep the boundary test on whole samples
    struct dsp_tdoa_estimator *est = dsp_tdoa_create(&cfg, N);

    // Just past d/c but within tolerance: accepted, clamped to -90 degrees
    make_pair(a, b, 35, 0.02, 55u, 1);
    struct dsp_tdoa_result r = run(est, a, b, N);
    CHECK(r.status == DSP_TDOA_OK, "%s delay 35: %s", method_name(m), dsp_tdoa_status_str(r.status));
    CHECK(r.flags & DSP_TDOA_FLAG_CLAMPED, "%s delay 35: expected clamp flag, flags %x", method_name(m), r.flags);
    CHECK(fabs(r.angle_deg + 90.0) < 1e-9, "%s delay 35: angle %f", method_name(m), r.angle_deg);

    // Far outside: strongest peak is out of range and must not become an angle
    make_pair(a, b, -60, 0.02, 56u, 1);
    r = run(est, a, b, N);
    CHECK(r.status == DSP_TDOA_PEAK_OUT_OF_RANGE, "%s delay -60: %s", method_name(m), dsp_tdoa_status_str(r.status));
    CHECK(r.global_peak_lag == -60, "%s delay -60: global lag %d", method_name(m), r.global_peak_lag);
    CHECK(isnan(r.angle_deg) && isnan(r.tdoa_s), "%s delay -60: angle should be NaN", method_name(m));
    dsp_tdoa_destroy(est);
}

// A DC offset on one channel changes nothing when DC removal is on
static void test_dc_offset(enum dsp_tdoa_method m)
{
    struct dsp_tdoa_config cfg = test_config(m);
    struct dsp_tdoa_estimator *est = dsp_tdoa_create(&cfg, N);
    make_pair(a, b, -14, 0.02, 9u, 2);
    for (int i = 0; i < N; i++) {
        a[i] += 1000.0f;
    }
    struct dsp_tdoa_result r = run(est, a, b, N);
    CHECK(r.status == DSP_TDOA_OK && r.peak_lag == -14, "%s dc offset: %s lag %d",
          method_name(m), dsp_tdoa_status_str(r.status), r.peak_lag);
    dsp_tdoa_destroy(est);
}

// Both methods find the same lag on identical data
static void test_methods_agree(void)
{
    struct dsp_tdoa_config cx = test_config(DSP_TDOA_XCORR);
    struct dsp_tdoa_config cg = test_config(DSP_TDOA_GCC_PHAT);
    struct dsp_tdoa_estimator *ex = dsp_tdoa_create(&cx, N);
    struct dsp_tdoa_estimator *eg = dsp_tdoa_create(&cg, N);
    for (int d = -34; d <= 34; d += 2) {
        make_pair(a, b, d, 0.05, 400u + (unsigned long)(d + 40), 2);
        struct dsp_tdoa_result rx = run(ex, a, b, N);
        struct dsp_tdoa_result rg = run(eg, a, b, N);
        CHECK(rx.peak_lag == d && rg.peak_lag == d, "delay %d: xcorr %d gcc %d", d, rx.peak_lag, rg.peak_lag);
    }
    dsp_tdoa_destroy(ex);
    dsp_tdoa_destroy(eg);
}

// Silence, one dead channel, and noise with no impact
static void test_no_event(enum dsp_tdoa_method m)
{
    struct dsp_tdoa_config cfg = test_config(m);
    struct dsp_tdoa_estimator *est = dsp_tdoa_create(&cfg, N);

    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    struct dsp_tdoa_result r = run(est, a, b, N);
    CHECK(r.status == DSP_TDOA_LOW_ENERGY, "%s silence: %s", method_name(m), dsp_tdoa_status_str(r.status));
    CHECK(isnan(r.angle_deg), "%s silence: angle not NaN", method_name(m));

    // Constant (DC only) is silence after DC removal
    for (int i = 0; i < N; i++) {
        a[i] = 512.0f;
        b[i] = 2048.0f;
    }
    r = run(est, a, b, N);
    CHECK(r.status == DSP_TDOA_LOW_ENERGY, "%s constant: %s", method_name(m), dsp_tdoa_status_str(r.status));

    make_pair(a, b, 5, 0.0, 3u, 1);
    memset(b, 0, sizeof(b));
    r = run(est, a, b, N);
    CHECK(r.status == DSP_TDOA_LOW_ENERGY, "%s dead channel B: %s", method_name(m), dsp_tdoa_status_str(r.status));

    // Independent noise on each channel: must never produce a valid result
    int valid = 0;
    for (unsigned long seed = 1; seed <= 200; seed++) {
        rng_state = seed * 7919u;
        for (int i = 0; i < N; i++) {
            a[i] = (float)urand();
            b[i] = (float)urand();
        }
        r = run(est, a, b, N);
        CHECK(isfinite(r.peak_to_rms) && isfinite(r.confidence), "%s noise seed %lu: non-finite diagnostics",
              method_name(m), seed);
        if (r.status == DSP_TDOA_OK) {
            valid++;
        }
    }
    CHECK(valid == 0, "%s noise only: %d/200 windows reported valid", method_name(m), valid);
    dsp_tdoa_destroy(est);
}

// Heavy noise: the estimator may refuse, but must not report a wrong delay as valid
static void test_noisy_impacts(enum dsp_tdoa_method m)
{
    struct dsp_tdoa_config cfg = test_config(m);
    struct dsp_tdoa_estimator *est = dsp_tdoa_create(&cfg, N);
    const double noises[] = { 0.1, 0.3, 0.6 };
    for (size_t ni = 0; ni < 3; ni++) {
        for (int smooth = 1; smooth <= 3; smooth += 2) {
            int ok = 0, wrong = 0;
            for (unsigned long seed = 1; seed <= 100; seed++) {
                int d = (int)(seed % 69) - 34;
                make_pair(a, b, d, noises[ni], seed, smooth);
                struct dsp_tdoa_result r = run(est, a, b, N);
                if (r.status == DSP_TDOA_OK) {
                    ok++;
                    if (fabs(r.raw_lag_samples - d) > 1.0) {
                        wrong++;
                    }
                }
            }
            printf("  %-8s noise %.1f smooth %d: %3d/100 valid, %d valid-but-wrong\n",
                   method_name(m), noises[ni], smooth, ok, wrong);
            CHECK(wrong == 0, "%s noise %.1f smooth %d: %d confident wrong delays",
                  method_name(m), noises[ni], smooth, wrong);
        }
    }
    dsp_tdoa_destroy(est);
}

static void test_bad_input(void)
{
    struct dsp_tdoa_config cfg = test_config(DSP_TDOA_GCC_PHAT);
    struct dsp_tdoa_estimator *est = dsp_tdoa_create(&cfg, N);
    struct dsp_tdoa_result r;
    make_pair(a, b, 4, 0.02, 12u, 1);

    struct dsp_tdoa_input in = { .a = a, .b = NULL, .n = N, .clipped = false };
    CHECK(dsp_tdoa_estimate(est, &in, &r) == DSP_TDOA_ERR_ARGS, "null b");
    CHECK(isnan(r.angle_deg), "null b: angle not NaN");
    in.b = b;
    in.n = 0;
    CHECK(dsp_tdoa_estimate(est, &in, &r) == DSP_TDOA_ERR_ARGS, "n = 0");
    in.n = N + 1;
    CHECK(dsp_tdoa_estimate(est, &in, &r) == DSP_TDOA_ERR_ARGS, "n > max");
    in.n = 20; // shorter than the +-36 sample physical lag range
    CHECK(dsp_tdoa_estimate(est, &in, &r) == DSP_TDOA_ERR_ARGS, "window too short");
    in.n = N;
    CHECK(dsp_tdoa_estimate(NULL, &in, &r) == DSP_TDOA_ERR_ARGS, "null estimator");
    CHECK(dsp_tdoa_estimate(est, NULL, &r) == DSP_TDOA_ERR_ARGS, "null input");
    CHECK(dsp_tdoa_estimate(est, &in, NULL) == DSP_TDOA_ERR_ARGS, "null result");

    a[100] = NAN;
    CHECK(dsp_tdoa_estimate(est, &in, &r) == DSP_TDOA_ERR_NONFINITE, "NaN sample");
    a[100] = INFINITY;
    CHECK(dsp_tdoa_estimate(est, &in, &r) == DSP_TDOA_ERR_NONFINITE, "Inf sample");
    a[100] = 0.0f;

    // Clipping is passed through as a flag
    in.clipped = true;
    CHECK(dsp_tdoa_estimate(est, &in, &r) == DSP_TDOA_OK && (r.flags & DSP_TDOA_FLAG_CLIPPED), "clip flag");
    dsp_tdoa_destroy(est);

    // Invalid configurations are refused
    struct dsp_tdoa_config bad;
    dsp_tdoa_default_config(&bad);
    CHECK(dsp_tdoa_create(&bad, N) == NULL, "defaults without fs/spacing must be refused");
    bad = test_config(DSP_TDOA_GCC_PHAT);
    bad.mic_spacing_m = -0.1;
    CHECK(dsp_tdoa_create(&bad, N) == NULL, "negative spacing");
    bad = test_config(DSP_TDOA_GCC_PHAT);
    bad.speed_of_sound_mps = NAN;
    CHECK(dsp_tdoa_create(&bad, N) == NULL, "NaN speed of sound");
    bad = test_config(DSP_TDOA_GCC_PHAT);
    CHECK(dsp_tdoa_create(&bad, 1) == NULL, "max_samples = 1");
    CHECK(dsp_tdoa_create(NULL, N) == NULL, "null config");
}

// Correlation export for plotting has the documented shape
static void test_correlation_export(void)
{
    struct dsp_tdoa_config cfg = test_config(DSP_TDOA_XCORR);
    struct dsp_tdoa_estimator *est = dsp_tdoa_create(&cfg, N);
    int first = 0;
    size_t len = 0;
    CHECK(dsp_tdoa_correlation(est, &first, &len) == NULL, "no correlation before first estimate");
    make_pair(a, b, 6, 0.0, 5u, 1);
    struct dsp_tdoa_result r = run(est, a, b, N);
    const double *c = dsp_tdoa_correlation(est, &first, &len);
    CHECK(c != NULL && first == -(N - 1) && len == 2 * N - 1, "shape first %d len %zu", first, len);
    CHECK(c != NULL && c[6 - first] == r.peak_value, "peak value at lag 6");
    dsp_tdoa_destroy(est);
}

int main(void)
{
    const enum dsp_tdoa_method methods[] = { DSP_TDOA_XCORR, DSP_TDOA_GCC_PHAT };
    for (size_t i = 0; i < 2; i++) {
        test_known_delays(methods[i]);
        test_channel_swap(methods[i]);
        test_skew(methods[i]);
        test_physical_limits(methods[i]);
        test_dc_offset(methods[i]);
        test_no_event(methods[i]);
        test_noisy_impacts(methods[i]);
    }
    test_methods_agree();
    test_bad_input();
    test_correlation_export();

    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures;
}
