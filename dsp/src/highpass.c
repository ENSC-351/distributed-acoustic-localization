// See dsp/highpass.h. RBJ audio-EQ-cookbook high-pass biquad.
#include "dsp/highpass.h"

#include <math.h>

#define MID_SCALE 2048.0
#define FULL_SCALE 4095.0
#define PI 3.14159265358979323846

bool dsp_highpass_init(struct dsp_highpass *hp, double cutoff_hz, double sample_rate_hz, int channels)
{
    if (hp == NULL || !(cutoff_hz > 0.0) || !(sample_rate_hz > 2.0 * cutoff_hz) || channels < 1 ||
        channels > DSP_HIGHPASS_MAX_CHANNELS) {
        return false;
    }
    double w0 = 2.0 * PI * cutoff_hz / sample_rate_hz;
    double alpha = sin(w0) / (2.0 * 0.7071067811865476);
    double c = cos(w0);
    double a0 = 1.0 + alpha;
    hp->b0 = (1.0 + c) / 2.0 / a0;
    hp->b1 = -(1.0 + c) / a0;
    hp->b2 = (1.0 + c) / 2.0 / a0;
    hp->a1 = -2.0 * c / a0;
    hp->a2 = (1.0 - alpha) / a0;
    hp->channels = channels;
    dsp_highpass_reset(hp);
    return true;
}

void dsp_highpass_reset(struct dsp_highpass *hp)
{
    hp->primed = false;
    for (int ch = 0; ch < DSP_HIGHPASS_MAX_CHANNELS; ch++) {
        hp->x1[ch] = hp->x2[ch] = hp->y1[ch] = hp->y2[ch] = 0.0;
    }
}

void dsp_highpass_process(struct dsp_highpass *hp, const uint16_t *in, uint16_t *out, size_t num_frames)
{
    int nc = hp->channels;
    if (!hp->primed && num_frames > 0) {
        // Start as if the first value had always been there: no step response
        for (int ch = 0; ch < nc; ch++) {
            hp->x1[ch] = hp->x2[ch] = in[ch];
            hp->y1[ch] = hp->y2[ch] = 0.0;
        }
        hp->primed = true;
    }
    for (size_t f = 0; f < num_frames; f++) {
        for (int ch = 0; ch < nc; ch++) {
            size_t i = f * (size_t)nc + (size_t)ch;
            double x = in[i];
            double y = hp->b0 * x + hp->b1 * hp->x1[ch] + hp->b2 * hp->x2[ch] - hp->a1 * hp->y1[ch] -
                       hp->a2 * hp->y2[ch];
            hp->x2[ch] = hp->x1[ch];
            hp->x1[ch] = x;
            hp->y2[ch] = hp->y1[ch];
            hp->y1[ch] = y;
            double v = round(y + MID_SCALE);
            out[i] = (uint16_t)(v < 0.0 ? 0.0 : (v > FULL_SCALE ? FULL_SCALE : v));
        }
    }
}
