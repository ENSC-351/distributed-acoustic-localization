// Second-order (Butterworth, Q = 0.707) high-pass filter for a stream of
// interleaved frames, one filter state per channel. Used before detection
// and TDOA estimation to remove low-frequency noise and hum, which make the
// correlation peak broad (measured on real claps, 2026-11-26: fewer
// out-of-range peaks and higher confidence with a 500 Hz cut-off).
//
// Works on ADC counts: output = filtered signal + 2048 (mid-scale), clamped
// to 0..4095, so the result still looks like ADC samples to the detector.
// Note: clipping in the input is no longer visible in the output.
#ifndef DSP_HIGHPASS_H
#define DSP_HIGHPASS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DSP_HIGHPASS_MAX_CHANNELS 8

struct dsp_highpass {
    double b0, b1, b2, a1, a2;
    int channels;
    bool primed;                               // states start from the first sample (no step)
    double x1[DSP_HIGHPASS_MAX_CHANNELS], x2[DSP_HIGHPASS_MAX_CHANNELS];
    double y1[DSP_HIGHPASS_MAX_CHANNELS], y2[DSP_HIGHPASS_MAX_CHANNELS];
};

// Returns false for a cut-off outside (0, fs/2) or a bad channel count
bool dsp_highpass_init(struct dsp_highpass *hp, double cutoff_hz, double sample_rate_hz, int channels);

// Filters num_frames interleaved frames from in to out (may be the same buffer)
void dsp_highpass_process(struct dsp_highpass *hp, const uint16_t *in, uint16_t *out, size_t num_frames);

// Forget the history (after a gap in the input)
void dsp_highpass_reset(struct dsp_highpass *hp);

#endif
