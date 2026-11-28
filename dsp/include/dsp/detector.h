// Streaming impact detector and window extractor (draft interface)
//
// C, streaming version of Tyler's context/tyler_dsp_v2/detector.py. Samples go
// in block by block as they are captured; aligned two-channel event windows
// come out, ready for dsp_tdoa_estimate() (see tdoa.h).
//
// Same rules as the Python version:
//   * The trigger comes from BOTH channels combined: energy = causal moving
//     average (smooth_ms) of (a - dcA)^2 + (b - dcB)^2.
//   * The SAME sample indices are cut from both channels. Neither channel is
//     ever shifted; channel skew is the estimator's job (dsp_tdoa_config).
//   * Trigger when energy > threshold_ratio * noise floor. The window is
//     pre_ms before the trigger to post_ms after it.
//   * After a trigger, ignore holdoff_ms, then wait until energy falls to
//     rearm_ratio * threshold or below before triggering again.
//   * Detection uses its own DC estimate (from calibration) and never changes
//     the output. Output samples have DC removed in one place only: the
//     median of the window's pre-trigger samples per channel (the whole
//     window's median if fewer than 8 pre-trigger samples).
//
// Differences from the Python version, because the input never ends:
//   * The noise model comes from calibration: the first calibrate_ms of
//     input (no triggers during it), or dsp_detector_set_noise().
//   * A discontinuity (blocks lost, or a block with disturbed timing) can't be
//     bridged: any window that would contain it is emitted as invalid, and
//     the energy average restarts after it.
//
// Usage on a node:
//     struct dsp_detector_config cfg;
//     dsp_detector_default_config(&cfg);
//     cfg.sample_rate_hz = AudioCapture_getSampleRateHz();
//     struct dsp_detector *det = dsp_detector_create(&cfg);
//     for each captured block:
//         dsp_detector_push(det, frames, info.numFrames, &block);
//         struct dsp_detector_window w;
//         while (dsp_detector_next(det, &w)) {
//             if (w.status == DSP_DETECTOR_OK) {
//                 struct dsp_tdoa_input in = dsp_detector_tdoa_input(&w);
//                 dsp_tdoa_estimate(est, &in, &result);
//             }
//         }
//
// No memory is allocated after dsp_detector_create(), so it can run on a
// real-time thread. One detector must not be used by two threads at once.
#ifndef DSP_DETECTOR_H
#define DSP_DETECTOR_H

#include "dsp/tdoa.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum dsp_detector_status {
    DSP_DETECTOR_OK         = 0, // complete, evenly spaced window
    DSP_DETECTOR_INCOMPLETE = 1, // the window would start before the first sample
                                 // (or before a discontinuity): not enough history
    DSP_DETECTOR_GAP        = 2, // a discontinuity falls inside the window
};

// Diagnostic flags on a window (do not change its status)
#define DSP_DETECTOR_FLAG_CLIPPED_A (1u << 0) // a raw sample of channel A is at or beyond adc_min/adc_max
#define DSP_DETECTOR_FLAG_CLIPPED_B (1u << 1)
#define DSP_DETECTOR_FLAG_SHORT_PRE (1u << 2) // < 8 pre-trigger samples: DC taken from the whole window

struct dsp_detector_config {
    double sample_rate_hz;   // per channel; MUST be set (default 0 is rejected)
    int    num_channels;     // samples per input frame (default 2)
    int    channel_a;        // index of mic A inside a frame (default 0)
    int    channel_b;        // index of mic B inside a frame (default 1)

    // Same names and defaults as Tyler's DetectorConfig
    double threshold_ratio;  // 12: trigger at this multiple of the noise-floor energy
    double smooth_ms;        // 0.5: moving-average length of the energy envelope
    double pre_ms;           // 5: kept before the trigger
    double post_ms;          // 15: kept after the trigger
    double holdoff_ms;       // 50: ignore triggers this long after one fires
    double rearm_ratio;      // 0.5: energy must fall below this * threshold to re-arm
    double min_noise_floor;  // 1e-9: avoids divide-by-zero on perfect silence

    // Clipping check on raw samples; set check_clipping false to disable
    bool   check_clipping;   // true
    double adc_min;          // 0
    double adc_max;          // 4095 (MCP3208, 12 bits)

    // Calibration from the start of the stream (ignored after set_noise)
    double calibrate_ms;     // 1000: no triggers until this much input has been seen
};

// Fixed DC and noise floor of the detection energy (Tyler's NoiseModel)
struct dsp_detector_noise {
    double dc_a, dc_b;       // median of each channel during calibration
    double floor_energy;     // median of the combined energy during calibration
};

// Describes one pushed block. Times use whatever clock the caller chooses
// (CLOCK_REALTIME on the node); the detector only does arithmetic on them.
struct dsp_detector_block {
    int64_t t_first_ns;      // time of the block's first frame (channel A's sample)
    double  frame_period_ns; // time between frames inside the block
    bool    discontinuity;   // frames were lost just before this block
    bool    disturbed;       // timing inside this block is not trustworthy
};

struct dsp_detector_window {
    enum dsp_detector_status status;
    uint32_t flags;          // DSP_DETECTOR_FLAG_*

    // Both arrays have n samples and the same frame indices. Owned by the
    // detector: valid until the next dsp_detector_push() or _next() call.
    // Invalid windows (status != OK) have n = 0 and NULL arrays.
    const float *a, *b;          // DC-removed
    const float *raw_a, *raw_b;  // as captured, same indices
    size_t n;                    // window length (pre + post samples)
    size_t trigger_index;        // index of the trigger inside the window

    uint64_t start_frame;    // frame number (counted from the first push) of a[0]
    int64_t  t_first_ns;     // time of a[0]: the window's first sample, not the trigger
    double   dc_a, dc_b;     // DC that was removed
    double   threshold_energy;
    double   peak_to_floor;  // largest energy inside the window / noise floor
};

// Fill in defaults. sample_rate_hz is set to 0 and MUST be set by the caller.
void dsp_detector_default_config(struct dsp_detector_config *cfg);

// True if every config field is in range
bool dsp_detector_config_valid(const struct dsp_detector_config *cfg);

struct dsp_detector;

// Allocates all working memory (history, calibration buffer, output windows).
// Returns NULL on an invalid config or out of memory.
struct dsp_detector *dsp_detector_create(const struct dsp_detector_config *cfg);
void dsp_detector_destroy(struct dsp_detector *det);

// Use this noise model instead of calibrating from the stream (for example
// one measured earlier, or in tests). Triggers are armed straight away.
void dsp_detector_set_noise(struct dsp_detector *det, const struct dsp_detector_noise *noise);

// True once a noise model exists; copies it to *noise if noise is non-NULL.
bool dsp_detector_get_noise(const struct dsp_detector *det, struct dsp_detector_noise *noise);

// Feeds num_frames frames, interleaved [f0 ch0, f0 ch1, ..., f1 ch0, ...],
// num_channels values each (the AudioCapture block layout). Raw ADC counts
// are fine. Returns the number of windows now waiting for dsp_detector_next().
// Windows are only emitted once their last (post-trigger) sample has arrived.
size_t dsp_detector_push(struct dsp_detector *det, const uint16_t *frames, size_t num_frames,
                         const struct dsp_detector_block *block);

// Same, for float samples (offline tools and tests with synthetic records)
size_t dsp_detector_push_float(struct dsp_detector *det, const float *frames, size_t num_frames,
                               const struct dsp_detector_block *block);

// Takes the oldest waiting window. Returns false if there are none. Invalid
// windows (status != OK) are returned too, so callers can count them.
bool dsp_detector_next(struct dsp_detector *det, struct dsp_detector_window *out);

// Windows dropped because the caller didn't take them before the queue filled
uint32_t dsp_detector_dropped(const struct dsp_detector *det);

// Forget all history and pending windows, keeping the noise model (for
// example after capture restarts). The next push counts as a discontinuity.
void dsp_detector_reset(struct dsp_detector *det);

const char *dsp_detector_status_str(enum dsp_detector_status status);

// Builds the estimator input from an OK window
static inline struct dsp_tdoa_input dsp_detector_tdoa_input(const struct dsp_detector_window *w)
{
    struct dsp_tdoa_input in = {
        .a = w->a,
        .b = w->b,
        .n = w->n,
        .clipped = (w->flags & (DSP_DETECTOR_FLAG_CLIPPED_A | DSP_DETECTOR_FLAG_CLIPPED_B)) != 0,
    };
    return in;
}

#endif
