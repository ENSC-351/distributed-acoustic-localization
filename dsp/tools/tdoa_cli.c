// tdoa_cli: run the TDOA estimator on one two-channel window read from a file
//
// Usage:
//   tdoa_cli --fs HZ --spacing M [--skew S] [--c MPS] [--method xcorr|gcc]
//            [--no-dc] [--no-interp] [--min-psr X] FILE
//
// FILE has one sample pair per line: "a,b" (commas, spaces, tabs or
// semicolons). Lines starting with '#' and a non-numeric header are skipped.
// Use "-" for stdin. Prints one JSON object on stdout. Exit code is 0 when the
// estimate is valid, 1 when the estimator refused, 2 on a usage or file error.
#include "dsp/tdoa.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE    256
#define MAX_SAMPLES (1u << 20)

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --fs HZ --spacing M [--skew S] [--c MPS] [--method xcorr|gcc]\n"
            "          [--no-dc] [--no-interp] [--min-psr X] FILE\n"
            "FILE: one 'a,b' pair per line, '-' for stdin\n", prog);
}

static bool parse_double(const char *text, double *out)
{
    char *end = NULL;
    double v = strtod(text, &end);
    if (end == text || *end != '\0' || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

// Reads pairs into growing arrays. Returns the count, or 0 on error.
static size_t read_pairs(FILE *f, float **pa, float **pb)
{
    size_t n = 0, cap = 0;
    float *a = NULL, *b = NULL;
    char line[MAX_LINE];
    size_t lineno = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') {
            continue;
        }
        char *end1 = NULL, *end2 = NULL;
        double va = strtod(p, &end1);
        char *q = end1;
        while (*q == ',' || *q == ' ' || *q == '\t' || *q == ';') {
            q++;
        }
        double vb = strtod(q, &end2);
        if (end1 == p || end2 == q) {
            if (n == 0) {
                continue; // header line
            }
            fprintf(stderr, "line %zu: expected two numbers\n", lineno);
            free(a);
            free(b);
            return 0;
        }
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 1024;
            if (ncap > MAX_SAMPLES) {
                fprintf(stderr, "more than %u samples\n", MAX_SAMPLES);
                free(a);
                free(b);
                return 0;
            }
            float *na = realloc(a, ncap * sizeof(*a));
            float *nb = realloc(b, ncap * sizeof(*b));
            if (na == NULL || nb == NULL) {
                free(na ? na : a);
                free(nb ? nb : b);
                fprintf(stderr, "out of memory\n");
                return 0;
            }
            a = na;
            b = nb;
            cap = ncap;
        }
        a[n] = (float)va;
        b[n] = (float)vb;
        n++;
    }
    *pa = a;
    *pb = b;
    return n;
}

// JSON has no NaN, so print null instead
static void json_num(const char *key, double v, bool last)
{
    if (isfinite(v)) {
        printf("  \"%s\": %.9g%s\n", key, v, last ? "" : ",");
    } else {
        printf("  \"%s\": null%s\n", key, last ? "" : ",");
    }
}

int main(int argc, char *argv[])
{
    struct dsp_tdoa_config cfg;
    dsp_tdoa_default_config(&cfg);
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
        double *target = NULL;
        if (strcmp(arg, "--fs") == 0) {
            target = &cfg.sample_rate_hz;
        } else if (strcmp(arg, "--spacing") == 0) {
            target = &cfg.mic_spacing_m;
        } else if (strcmp(arg, "--skew") == 0) {
            target = &cfg.channel_skew_s;
        } else if (strcmp(arg, "--c") == 0) {
            target = &cfg.speed_of_sound_mps;
        } else if (strcmp(arg, "--min-psr") == 0) {
            target = &cfg.min_peak_to_rms;
        } else if (strcmp(arg, "--method") == 0 && val != NULL) {
            if (strcmp(val, "xcorr") == 0) {
                cfg.method = DSP_TDOA_XCORR;
            } else if (strcmp(val, "gcc") == 0) {
                cfg.method = DSP_TDOA_GCC_PHAT;
            } else {
                usage(argv[0]);
                return 2;
            }
            i++;
            continue;
        } else if (strcmp(arg, "--no-dc") == 0) {
            cfg.remove_dc = false;
            continue;
        } else if (strcmp(arg, "--no-interp") == 0) {
            cfg.interpolate = false;
            continue;
        } else if (path == NULL && (arg[0] != '-' || strcmp(arg, "-") == 0)) {
            path = arg;
            continue;
        } else {
            usage(argv[0]);
            return 2;
        }
        if (val == NULL || !parse_double(val, target)) {
            fprintf(stderr, "%s needs a number\n", arg);
            return 2;
        }
        i++;
    }
    if (path == NULL || !dsp_tdoa_config_valid(&cfg)) {
        usage(argv[0]);
        if (path != NULL) {
            fprintf(stderr, "invalid settings (--fs and --spacing are required)\n");
        }
        return 2;
    }

    FILE *f = (strcmp(path, "-") == 0) ? stdin : fopen(path, "r");
    if (f == NULL) {
        perror(path);
        return 2;
    }
    float *a = NULL, *b = NULL;
    size_t n = read_pairs(f, &a, &b);
    if (f != stdin) {
        fclose(f);
    }
    if (n < 2) {
        fprintf(stderr, "%s: need at least 2 sample pairs\n", path);
        free(a);
        free(b);
        return 2;
    }

    struct dsp_tdoa_estimator *est = dsp_tdoa_create(&cfg, n);
    if (est == NULL) {
        fprintf(stderr, "could not create estimator\n");
        free(a);
        free(b);
        return 2;
    }
    struct dsp_tdoa_input in = { .a = a, .b = b, .n = n, .clipped = false };
    struct dsp_tdoa_result r;
    dsp_tdoa_estimate(est, &in, &r);

    printf("{\n");
    printf("  \"status\": \"%s\",\n", dsp_tdoa_status_str(r.status));
    printf("  \"valid\": %s,\n", r.status == DSP_TDOA_OK ? "true" : "false");
    printf("  \"method\": \"%s\",\n", cfg.method == DSP_TDOA_XCORR ? "xcorr" : "gcc-phat");
    printf("  \"samples\": %zu,\n", n);
    json_num("sample_rate_hz", cfg.sample_rate_hz, false);
    json_num("mic_spacing_m", cfg.mic_spacing_m, false);
    json_num("channel_skew_s", cfg.channel_skew_s, false);
    json_num("tdoa_s", r.tdoa_s, false);
    json_num("angle_deg", r.angle_deg, false);
    json_num("confidence", r.confidence, false);
    json_num("raw_lag_samples", r.raw_lag_samples, false);
    printf("  \"peak_lag\": %d,\n", r.peak_lag);
    printf("  \"global_peak_lag\": %d,\n", r.global_peak_lag);
    printf("  \"min_lag\": %d,\n", r.min_lag);
    printf("  \"max_lag\": %d,\n", r.max_lag);
    json_num("peak_to_rms", r.peak_to_rms, false);
    json_num("second_peak_ratio", r.peak_value > 0.0 ? r.second_peak_value / r.peak_value : NAN, false);
    printf("  \"flags\": %u\n", (unsigned)r.flags);
    printf("}\n");

    dsp_tdoa_destroy(est);
    free(a);
    free(b);
    return r.status == DSP_TDOA_OK ? 0 : 1;
}
