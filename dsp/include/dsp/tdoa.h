// Two-microphone TDOA and direction estimator (draft)
//
// Takes one aligned two-channel event window and returns the physical arrival
// delay between the microphones and a far-field bearing, or an explicit reason
// why no valid result was produced. See dsp/README.md for the conventions.
//
// Conventions (must match the rest of the pipeline):
//   tdoa_s  = t_B - t_A, physical arrival times. Positive: sound reached A first.
//   skew    = sampling_time_B - sampling_time_A for the same frame index.
//   Lag     r[k] = sum_n a[n] * b[n + k]. If b is a copy of a delayed by D
//           samples, the peak is at k = +D.
//   tdoa_s  = peak_lag / fs + skew
//   Mic A at x = -d/2, mic B at x = +d/2. theta = 0 is broadside, positive
//   toward B, in the forward half-plane: theta = asin(-c * tdoa / d).
//
// No memory is allocated after dsp_tdoa_create(), so estimates can run on a
// real-time thread. One estimator must not be used by two threads at once.
#ifndef DSP_TDOA_H
#define DSP_TDOA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum dsp_tdoa_method {
    DSP_TDOA_XCORR    = 0, // plain time-domain cross-correlation (baseline)
    DSP_TDOA_GCC_PHAT = 1, // FFT cross-spectrum with PHAT weighting
};

enum dsp_tdoa_status {
    DSP_TDOA_OK                = 0,
    DSP_TDOA_ERR_ARGS          = 1, // null pointer, bad length, or window shorter than the lag range
    DSP_TDOA_ERR_NONFINITE     = 2, // NaN or Inf in the samples
    DSP_TDOA_LOW_ENERGY        = 3, // a channel's RMS is at or below min_rms
    DSP_TDOA_NO_PEAK           = 4, // no positive correlation peak in the physical lag range
    DSP_TDOA_PEAK_OUT_OF_RANGE = 5, // strongest peak overall is outside the physical lag range
    DSP_TDOA_TDOA_OUT_OF_RANGE = 6, // corrected delay exceeds d/c plus tolerance
    DSP_TDOA_WEAK_PEAK         = 7, // peak_to_rms below min_peak_to_rms (looks like noise)
};

// Diagnostic flags (do not change the status on their own)
#define DSP_TDOA_FLAG_CLIPPED   (1u << 0) // caller reported clipping in the input window
#define DSP_TDOA_FLAG_AMBIGUOUS (1u << 1) // second peak >= ambiguity_ratio * main peak
#define DSP_TDOA_FLAG_AT_BOUND  (1u << 2) // chosen peak is on the edge of the allowed lag range
#define DSP_TDOA_FLAG_CLAMPED   (1u << 3) // asin input was just past +-1 and was clamped

struct dsp_tdoa_config {
    double sample_rate_hz;          // effective PER-CHANNEL rate, not total ADC conversions/s
    double mic_spacing_m;           // centre-to-centre spacing d
    double speed_of_sound_mps;      // default 343 (about 20 C)
    double channel_skew_s;          // sampling_time_B - sampling_time_A (MCP3208 reads channels in turn)
    double bound_tolerance_samples; // slack on |tdoa| <= d/c, for quantization and calibration error
    enum dsp_tdoa_method method;
    bool   remove_dc;               // subtract each channel's window mean before correlating
    bool   interpolate;             // parabolic sub-sample peak refinement
    double min_rms;                 // per channel after DC removal, in input units; 0 only rejects exact silence
    double ambiguity_ratio;         // in (0, 1]: flag when second peak / main peak >= this
    double min_peak_to_rms;         // reject peaks below this many correlation RMS; 0 disables.
                                    // Default 7 is from SYNTHETIC data (noise-only max ~5.7); retune on recordings.
    double phat_floor;              // in [0, 1): PHAT magnitude floor relative to the largest bin
};

struct dsp_tdoa_input {
    const float *a;   // channel A samples
    const float *b;   // channel B samples, same frame indices as a
    size_t n;         // samples per channel
    bool clipped;     // set by the caller if either channel clipped
};

struct dsp_tdoa_result {
    enum dsp_tdoa_status status;
    uint32_t flags;           // DSP_TDOA_FLAG_*

    // Only meaningful when status == DSP_TDOA_OK (NaN otherwise)
    double tdoa_s;            // corrected physical t_B - t_A
    double angle_deg;         // see conventions above, range [-90, 90]

    // Heuristic quality in [0, 1]: 1 - second_peak / main_peak. NOT a calibrated probability.
    double confidence;

    // Diagnostics (filled in as far as processing got)
    double raw_lag_samples;   // peak lag incl. sub-sample part, before skew correction
    int    peak_lag;          // integer lag of the chosen (in-range) peak
    int    global_peak_lag;   // integer lag of the strongest peak at any lag
    int    min_lag, max_lag;  // allowed lag range that was searched
    double peak_value;
    double second_peak_value;
    double global_peak_value;
    double peak_to_rms;       // chosen peak / RMS of the whole correlation
    double skew_applied_s;
    double rms_a, rms_b;      // after DC removal if enabled
};

// Fill in defaults. sample_rate_hz and mic_spacing_m are set to 0 and MUST be
// set by the caller; dsp_tdoa_create() rejects them otherwise.
void dsp_tdoa_default_config(struct dsp_tdoa_config *cfg);

// True if every config field is in range
bool dsp_tdoa_config_valid(const struct dsp_tdoa_config *cfg);

struct dsp_tdoa_estimator;

// Allocates all working memory for windows up to max_samples per channel.
// Returns NULL on an invalid config, max_samples < 2, or out of memory.
struct dsp_tdoa_estimator *dsp_tdoa_create(const struct dsp_tdoa_config *cfg, size_t max_samples);
void dsp_tdoa_destroy(struct dsp_tdoa_estimator *est);

// Estimate the delay and direction for one window. Always fills *out when out
// is non-NULL, and returns out->status.
enum dsp_tdoa_status dsp_tdoa_estimate(struct dsp_tdoa_estimator *est,
                                       const struct dsp_tdoa_input *in,
                                       struct dsp_tdoa_result *out);

// The correlation from the last estimate, for plots and debugging. Sets
// *first_lag to the lag of element 0 and *len to the number of elements.
// Returns NULL if no correlation has been computed yet. Valid until the next
// estimate.
const double *dsp_tdoa_correlation(const struct dsp_tdoa_estimator *est, int *first_lag, size_t *len);

const char *dsp_tdoa_status_str(enum dsp_tdoa_status status);

#endif
