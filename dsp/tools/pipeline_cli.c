// pipeline_cli: runs the node's DSP (detector -> TDOA estimator) over a
// recording, as fast as possible, with settings from the command line. For
// tuning thresholds on real recordings (the node uses the same code).
//
// Usage: pipeline_cli <file.csv> [key=value ...]
//   file: one frame per line, "a,b" (ADC counts), e.g. from a WAV via numpy
//   keys (defaults are the node's):
//     fs=20000 spacing=0.27 skew_us=17.52 method=xcorr|phat
//     threshold=12 smooth_ms=0.5 pre_ms=5 post_ms=15 holdoff_ms=50 rearm=0.5 calib_ms=1000
//     min_ptr=7 (estimator min peak-to-RMS) min_rms=0
//     hp=0 (high-pass cut-off Hz before everything; 0 = off)
//     quiet_s=0  (report windows that start before this time as false triggers)
// Output: one line per window (time s, status, theta, confidence, peak/rms,
// raw lag), then a summary.
#include "dsp/detector.h"
#include "dsp/highpass.h"
#include "dsp/tdoa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t *read_csv(const char *path, size_t *n_out)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror(path);
        return NULL;
    }
    size_t cap = 1 << 16, n = 0;
    uint16_t *v = malloc(cap * 2 * sizeof(uint16_t));
    char line[128];
    while (v != NULL && fgets(line, sizeof(line), f)) {
        int a, b;
        if (line[0] == '#' || sscanf(line, "%d,%d", &a, &b) != 2) {
            continue;
        }
        if (n == cap) {
            cap *= 2;
            uint16_t *g = realloc(v, cap * 2 * sizeof(uint16_t));
            if (g == NULL) {
                free(v);
                v = NULL;
                break;
            }
            v = g;
        }
        v[2 * n] = (uint16_t)a;
        v[2 * n + 1] = (uint16_t)b;
        n++;
    }
    fclose(f);
    *n_out = n;
    return v;
}

static double arg(int argc, char **argv, const char *key, double fallback)
{
    size_t len = strlen(key);
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], key, len) == 0 && argv[i][len] == '=') {
            return atof(argv[i] + len + 1);
        }
    }
    return fallback;
}

static const char *sarg(int argc, char **argv, const char *key, const char *fallback)
{
    size_t len = strlen(key);
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], key, len) == 0 && argv[i][len] == '=') {
            return argv[i] + len + 1;
        }
    }
    return fallback;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.csv> [key=value ...] (see source for keys)\n", argv[0]);
        return 2;
    }
    size_t n = 0;
    uint16_t *frames = read_csv(argv[1], &n);
    if (frames == NULL) {
        return 1;
    }
    double fs = arg(argc, argv, "fs", 20000.0);

    struct dsp_detector_config d;
    dsp_detector_default_config(&d);
    d.sample_rate_hz = fs;
    d.threshold_ratio = arg(argc, argv, "threshold", d.threshold_ratio);
    d.smooth_ms = arg(argc, argv, "smooth_ms", d.smooth_ms);
    d.pre_ms = arg(argc, argv, "pre_ms", d.pre_ms);
    d.post_ms = arg(argc, argv, "post_ms", d.post_ms);
    d.holdoff_ms = arg(argc, argv, "holdoff_ms", d.holdoff_ms);
    d.rearm_ratio = arg(argc, argv, "rearm", d.rearm_ratio);
    d.calibrate_ms = arg(argc, argv, "calib_ms", d.calibrate_ms);

    struct dsp_tdoa_config t;
    dsp_tdoa_default_config(&t);
    t.sample_rate_hz = fs;
    t.mic_spacing_m = arg(argc, argv, "spacing", 0.27);
    t.channel_skew_s = arg(argc, argv, "skew_us", 17.52) * 1e-6;
    t.method = strcmp(sarg(argc, argv, "method", "xcorr"), "phat") == 0 ? DSP_TDOA_GCC_PHAT : DSP_TDOA_XCORR;
    t.min_peak_to_rms = arg(argc, argv, "min_ptr", t.min_peak_to_rms);
    t.min_rms = arg(argc, argv, "min_rms", t.min_rms);
    double quiet_s = arg(argc, argv, "quiet_s", 0.0);
    double hp_hz = arg(argc, argv, "hp", 0.0);
    struct dsp_highpass hp;
    bool use_hp = hp_hz > 0.0 && dsp_highpass_init(&hp, hp_hz, fs, 2);
    if (use_hp) {
        dsp_highpass_process(&hp, frames, frames, n);
    }

    struct dsp_detector *det = dsp_detector_create(&d);
    struct dsp_tdoa_estimator *est =
        dsp_tdoa_create(&t, (size_t)((d.pre_ms + d.post_ms) * 1e-3 * fs) + 2);
    if (det == NULL || est == NULL) {
        fprintf(stderr, "invalid settings\n");
        return 1;
    }

    int windows = 0, ok = 0, false_trig = 0;
    double period_ns = 1e9 / fs;
    const size_t block = 128;
    for (size_t f = 0; f < n; f += block) {
        size_t nf = (n - f < block) ? n - f : block;
        struct dsp_detector_block blk = {(int64_t)((double)f * period_ns), period_ns, false, false};
        dsp_detector_push(det, frames + 2 * f, nf, &blk);
        struct dsp_detector_window w;
        while (dsp_detector_next(det, &w)) {
            windows++;
            double ts = (double)(w.start_frame + w.trigger_index) / fs;
            if (ts < quiet_s) {
                false_trig++;
            }
            if (w.status != DSP_DETECTOR_OK) {
                printf("%8.3f  invalid (%s)\n", ts, dsp_detector_status_str(w.status));
                continue;
            }
            struct dsp_tdoa_input in = dsp_detector_tdoa_input(&w);
            struct dsp_tdoa_result r;
            dsp_tdoa_estimate(est, &in, &r);
            if (r.status == DSP_TDOA_OK) {
                ok++;
            }
            printf("%8.3f  %-38s theta %6.1f  conf %.2f  p/rms %5.1f  lag %6.2f  trig/floor %.0f%s\n", ts,
                   dsp_tdoa_status_str(r.status), r.angle_deg, r.confidence, r.peak_to_rms, r.raw_lag_samples,
                   w.peak_to_floor, in.clipped ? "  clipped" : "");
        }
    }
    struct dsp_detector_noise noise;
    if (dsp_detector_get_noise(det, &noise)) {
        printf("# noise: dc %.1f %.1f, floor energy %.1f\n", noise.dc_a, noise.dc_b, noise.floor_energy);
    }
    printf("# %d windows, %d directions, %d before %.1f s (false triggers)\n", windows, ok, false_trig, quiet_s);
    dsp_detector_destroy(det);
    dsp_tdoa_destroy(est);
    free(frames);
    return 0;
}
