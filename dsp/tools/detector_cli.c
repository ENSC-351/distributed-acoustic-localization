// detector_cli: runs the streaming detector over a recording in a text file
// and prints one line per window. For checking the C detector against
// Tyler's detector.py (dsp/tools/compare_detector.py) and for offline tuning.
//
// Usage: detector_cli <file> <sample rate Hz> [dc_a dc_b floor_energy] [block frames]
//   <file>: one frame per line, two numbers (channel A, channel B), separated
//           by a comma or whitespace. Lines starting with '#' are skipped.
//   With dc_a dc_b floor_energy the noise model is fixed (as detector.py with
//   a NoiseModel); without them the detector calibrates from the start of the
//   stream (calibrate_ms of input, no triggers meanwhile).
//   Block frames (default 128) only changes how the input is pushed; the
//   output must not depend on it.
//
// Output, one line per window:
//   status trigger_frame start_frame n dc_a dc_b flags peak_to_floor
#include "dsp/detector.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float *read_frames(const char *path, size_t *num_frames)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror(path);
        return NULL;
    }
    size_t cap = 4096, n = 0;
    float *frames = malloc(2 * cap * sizeof(float));
    char line[256];
    while (frames != NULL && fgets(line, sizeof(line), f) != NULL) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        for (char *p = line; *p; p++) {
            if (*p == ',') {
                *p = ' ';
            }
        }
        double a, b;
        if (sscanf(line, "%lf %lf", &a, &b) != 2) {
            continue;   // header line or junk
        }
        if (n == cap) {
            cap *= 2;
            float *grown = realloc(frames, 2 * cap * sizeof(float));
            if (grown == NULL) {
                free(frames);
                frames = NULL;
                break;
            }
            frames = grown;
        }
        frames[2 * n] = (float)a;
        frames[2 * n + 1] = (float)b;
        n++;
    }
    fclose(f);
    *num_frames = n;
    return frames;
}

int main(int argc, char *argv[])
{
    if (argc != 3 && argc != 4 && argc != 6 && argc != 7) {
        fprintf(stderr, "Usage: %s <file> <sample rate Hz> [dc_a dc_b floor_energy] [block frames]\n", argv[0]);
        return 2;
    }
    size_t n = 0;
    float *frames = read_frames(argv[1], &n);
    if (frames == NULL) {
        return 1;
    }
    struct dsp_detector_config cfg;
    dsp_detector_default_config(&cfg);
    cfg.sample_rate_hz = atof(argv[2]);
    cfg.check_clipping = false;   // synthetic files aren't in ADC counts
    struct dsp_detector *det = dsp_detector_create(&cfg);
    if (det == NULL) {
        fprintf(stderr, "invalid detector config\n");
        free(frames);
        return 1;
    }
    if (argc >= 6) {
        struct dsp_detector_noise noise = {atof(argv[3]), atof(argv[4]), atof(argv[5])};
        dsp_detector_set_noise(det, &noise);
    }
    size_t block = 128;
    if (argc == 4 || argc == 7) {
        block = (size_t)atol(argv[argc - 1]);
        if (block == 0) {
            block = 1;
        }
    }

    double period_ns = 1e9 / cfg.sample_rate_hz;
    for (size_t f = 0; f < n; f += block) {
        size_t nf = (n - f < block) ? n - f : block;
        struct dsp_detector_block blk = {(int64_t)((double)f * period_ns), period_ns, false, false};
        dsp_detector_push_float(det, frames + 2 * f, nf, &blk);
        struct dsp_detector_window w;
        while (dsp_detector_next(det, &w)) {
            printf("%s %llu %llu %zu %.6f %.6f %u %.3f\n", w.status == DSP_DETECTOR_OK ? "ok" : "invalid",
                   (unsigned long long)(w.start_frame + w.trigger_index), (unsigned long long)w.start_frame, w.n,
                   w.dc_a, w.dc_b, w.flags, w.peak_to_floor);
        }
    }
    dsp_detector_destroy(det);
    free(frames);
    return 0;
}
