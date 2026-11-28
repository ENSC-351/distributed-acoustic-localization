// Two-microphone TDOA and direction estimator
#include "dsp/tdoa.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DSP_PI 3.14159265358979323846

struct dsp_tdoa_estimator {
    struct dsp_tdoa_config cfg;
    size_t max_samples;
    size_t nfft_max;

    double *a, *b;              // conditioned channels [max_samples]
    double *re_a, *im_a;        // FFT work [nfft_max]
    double *re_b, *im_b;
    double *cos_t, *sin_t;      // twiddles for nfft_max [nfft_max / 2]
    double *corr;               // lags -(n-1)..(n-1) [2 * max_samples - 1]

    size_t corr_len;            // valid length from the last estimate (0 = none)
    int corr_first_lag;
};

void dsp_tdoa_default_config(struct dsp_tdoa_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->sample_rate_hz = 0.0;
    cfg->mic_spacing_m = 0.0;
    cfg->speed_of_sound_mps = 343.0;
    cfg->channel_skew_s = 0.0;
    cfg->bound_tolerance_samples = 1.0;
    cfg->method = DSP_TDOA_GCC_PHAT;
    cfg->remove_dc = true;
    cfg->interpolate = true;
    cfg->min_rms = 0.0;
    cfg->ambiguity_ratio = 0.8;
    cfg->min_peak_to_rms = 7.0;
    cfg->phat_floor = 1e-3;
}

static bool positive_finite(double v)
{
    return isfinite(v) && v > 0.0;
}

bool dsp_tdoa_config_valid(const struct dsp_tdoa_config *cfg)
{
    if (cfg == NULL) {
        return false;
    }
    return positive_finite(cfg->sample_rate_hz)
        && positive_finite(cfg->mic_spacing_m)
        && positive_finite(cfg->speed_of_sound_mps)
        && isfinite(cfg->channel_skew_s)
        && isfinite(cfg->bound_tolerance_samples) && cfg->bound_tolerance_samples >= 0.0
        && (cfg->method == DSP_TDOA_XCORR || cfg->method == DSP_TDOA_GCC_PHAT)
        && isfinite(cfg->min_rms) && cfg->min_rms >= 0.0
        && isfinite(cfg->ambiguity_ratio) && cfg->ambiguity_ratio > 0.0 && cfg->ambiguity_ratio <= 1.0
        && isfinite(cfg->min_peak_to_rms) && cfg->min_peak_to_rms >= 0.0
        && isfinite(cfg->phat_floor) && cfg->phat_floor >= 0.0 && cfg->phat_floor < 1.0;
}

static size_t next_pow2(size_t n)
{
    size_t p = 1;
    while (p < n) {
        p <<= 1;
    }
    return p;
}

struct dsp_tdoa_estimator *dsp_tdoa_create(const struct dsp_tdoa_config *cfg, size_t max_samples)
{
    // Lags are stored as int, so keep 2 * max_samples well inside INT_MAX
    if (!dsp_tdoa_config_valid(cfg) || max_samples < 2 || max_samples > (size_t)(INT_MAX / 4)) {
        return NULL;
    }

    struct dsp_tdoa_estimator *est = calloc(1, sizeof(*est));
    if (est == NULL) {
        return NULL;
    }
    est->cfg = *cfg;
    est->max_samples = max_samples;
    est->nfft_max = next_pow2(2 * max_samples - 1);

    size_t nf = est->nfft_max;
    est->a = calloc(max_samples, sizeof(double));
    est->b = calloc(max_samples, sizeof(double));
    est->re_a = calloc(nf, sizeof(double));
    est->im_a = calloc(nf, sizeof(double));
    est->re_b = calloc(nf, sizeof(double));
    est->im_b = calloc(nf, sizeof(double));
    est->cos_t = calloc(nf / 2, sizeof(double));
    est->sin_t = calloc(nf / 2, sizeof(double));
    est->corr = calloc(2 * max_samples - 1, sizeof(double));

    if (!est->a || !est->b || !est->re_a || !est->im_a || !est->re_b || !est->im_b
        || !est->cos_t || !est->sin_t || !est->corr) {
        dsp_tdoa_destroy(est);
        return NULL;
    }

    // Forward-transform twiddles: exp(-j 2 pi k / N)
    for (size_t k = 0; k < nf / 2; k++) {
        double w = 2.0 * DSP_PI * (double)k / (double)nf;
        est->cos_t[k] = cos(w);
        est->sin_t[k] = -sin(w);
    }
    return est;
}

void dsp_tdoa_destroy(struct dsp_tdoa_estimator *est)
{
    if (est == NULL) {
        return;
    }
    free(est->a);
    free(est->b);
    free(est->re_a);
    free(est->im_a);
    free(est->re_b);
    free(est->im_b);
    free(est->cos_t);
    free(est->sin_t);
    free(est->corr);
    free(est);
}

// In-place iterative radix-2 FFT. n is a power of two that divides nfft_max.
// The inverse is scaled by 1/n.
static void fft(const struct dsp_tdoa_estimator *est, double *re, double *im, size_t n, bool inverse)
{
    // Bit-reversal permutation
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    for (size_t len = 2; len <= n; len <<= 1) {
        size_t half = len / 2;
        size_t step = est->nfft_max / len;
        for (size_t i = 0; i < n; i += len) {
            for (size_t k = 0; k < half; k++) {
                double wr = est->cos_t[k * step];
                double wi = inverse ? -est->sin_t[k * step] : est->sin_t[k * step];
                size_t p = i + k;
                size_t q = p + half;
                double vr = re[q] * wr - im[q] * wi;
                double vi = re[q] * wi + im[q] * wr;
                re[q] = re[p] - vr;
                im[q] = im[p] - vi;
                re[p] += vr;
                im[p] += vi;
            }
        }
    }

    if (inverse) {
        double scale = 1.0 / (double)n;
        for (size_t i = 0; i < n; i++) {
            re[i] *= scale;
            im[i] *= scale;
        }
    }
}

// Copy one channel to double, optionally remove its mean, and return its RMS.
// Returns false if any sample is not finite.
static bool condition_channel(const float *src, double *dst, size_t n, bool remove_dc, double *rms)
{
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        if (!isfinite(src[i])) {
            return false;
        }
        dst[i] = (double)src[i];
        sum += dst[i];
    }

    double mean = remove_dc ? sum / (double)n : 0.0;
    double sq = 0.0;
    for (size_t i = 0; i < n; i++) {
        dst[i] -= mean;
        sq += dst[i] * dst[i];
    }
    *rms = sqrt(sq / (double)n);
    return true;
}

// r[k] = sum_i a[i] * b[i + k], stored at corr[k + n - 1]
static void xcorr_time(struct dsp_tdoa_estimator *est, size_t n)
{
    int last = (int)n - 1;
    for (int k = -last; k <= last; k++) {
        size_t i0 = (k < 0) ? (size_t)(-k) : 0;
        size_t i1 = (k > 0) ? n - (size_t)k : n;
        double s = 0.0;
        for (size_t i = i0; i < i1; i++) {
            s += est->a[i] * est->b[(ptrdiff_t)i + k];
        }
        est->corr[k + last] = s;
    }
}

// Same lag definition as xcorr_time, via conj(A) * B with PHAT weighting.
// Returns false if the cross-spectrum is all zero.
static bool xcorr_gcc_phat(struct dsp_tdoa_estimator *est, size_t n, double phat_floor)
{
    size_t nf = next_pow2(2 * n - 1); // enough zero padding for linear correlation

    for (size_t i = 0; i < nf; i++) {
        est->re_a[i] = (i < n) ? est->a[i] : 0.0;
        est->re_b[i] = (i < n) ? est->b[i] : 0.0;
        est->im_a[i] = 0.0;
        est->im_b[i] = 0.0;
    }
    fft(est, est->re_a, est->im_a, nf, false);
    fft(est, est->re_b, est->im_b, nf, false);

    // Cross-spectrum conj(A) * B, kept in re_a/im_a
    double max_mag = 0.0;
    for (size_t i = 0; i < nf; i++) {
        double ar = est->re_a[i], ai = est->im_a[i];
        double br = est->re_b[i], bi = est->im_b[i];
        double xr = ar * br + ai * bi;
        double xi = ar * bi - ai * br;
        est->re_a[i] = xr;
        est->im_a[i] = xi;
        double mag = hypot(xr, xi);
        if (mag > max_mag) {
            max_mag = mag;
        }
    }
    if (!(max_mag > 0.0)) {
        return false;
    }

    double floor_mag = phat_floor * max_mag;
    if (floor_mag < DBL_MIN) {
        floor_mag = DBL_MIN;
    }
    for (size_t i = 0; i < nf; i++) {
        double mag = hypot(est->re_a[i], est->im_a[i]);
        double w = 1.0 / (mag > floor_mag ? mag : floor_mag);
        est->re_a[i] *= w;
        est->im_a[i] *= w;
    }
    fft(est, est->re_a, est->im_a, nf, true);

    // Circular index -> signed lag
    int last = (int)n - 1;
    for (int k = -last; k <= last; k++) {
        size_t idx = (k >= 0) ? (size_t)k : nf - (size_t)(-k);
        est->corr[k + last] = est->re_a[idx];
    }
    return true;
}

static void reset_result(struct dsp_tdoa_result *out)
{
    memset(out, 0, sizeof(*out));
    out->tdoa_s = NAN;
    out->angle_deg = NAN;
    out->raw_lag_samples = NAN;
}

enum dsp_tdoa_status dsp_tdoa_estimate(struct dsp_tdoa_estimator *est,
                                       const struct dsp_tdoa_input *in,
                                       struct dsp_tdoa_result *out)
{
    if (out == NULL) {
        return DSP_TDOA_ERR_ARGS;
    }
    reset_result(out);

    if (est == NULL || in == NULL || in->a == NULL || in->b == NULL
        || in->n < 2 || in->n > est->max_samples) {
        out->status = DSP_TDOA_ERR_ARGS;
        return out->status;
    }
    est->corr_len = 0;

    const struct dsp_tdoa_config *cfg = &est->cfg;
    const size_t n = in->n;
    const int last = (int)n - 1;
    const double fs = cfg->sample_rate_hz;
    const double skew = cfg->channel_skew_s;

    if (in->clipped) {
        out->flags |= DSP_TDOA_FLAG_CLIPPED;
    }

    // Allowed integer lags: physical |tdoa| <= d/c + tolerance, shifted by skew
    const double max_tdoa = cfg->mic_spacing_m / cfg->speed_of_sound_mps
                          + cfg->bound_tolerance_samples / fs;
    const double lo_f = ceil((-max_tdoa - skew) * fs - 1e-9);
    const double hi_f = floor((max_tdoa - skew) * fs + 1e-9);
    if (lo_f < (double)-last || hi_f > (double)last || lo_f > hi_f) {
        out->status = DSP_TDOA_ERR_ARGS; // window too short for the physical lag range
        return out->status;
    }
    const int lo = (int)lo_f;
    const int hi = (int)hi_f;
    out->min_lag = lo;
    out->max_lag = hi;

    // Conditioning
    if (!condition_channel(in->a, est->a, n, cfg->remove_dc, &out->rms_a)
        || !condition_channel(in->b, est->b, n, cfg->remove_dc, &out->rms_b)) {
        out->status = DSP_TDOA_ERR_NONFINITE;
        return out->status;
    }
    if (out->rms_a <= cfg->min_rms || out->rms_b <= cfg->min_rms) {
        out->status = DSP_TDOA_LOW_ENERGY;
        return out->status;
    }

    // Correlation over every lag
    if (cfg->method == DSP_TDOA_XCORR) {
        xcorr_time(est, n);
    } else if (!xcorr_gcc_phat(est, n, cfg->phat_floor)) {
        out->status = DSP_TDOA_LOW_ENERGY;
        return out->status;
    }
    est->corr_len = 2 * n - 1;
    est->corr_first_lag = -last;
    const double *r = est->corr + last; // r[k] for k in [-last, last]

    // Strongest peak inside the allowed range, and anywhere
    int p = lo;
    for (int k = lo + 1; k <= hi; k++) {
        if (r[k] > r[p]) {
            p = k;
        }
    }
    int g = -last;
    for (int k = -last + 1; k <= last; k++) {
        if (r[k] > r[g]) {
            g = k;
        }
    }
    out->peak_lag = p;
    out->peak_value = r[p];
    out->global_peak_lag = g;
    out->global_peak_value = r[g];

    double sq = 0.0;
    for (int k = -last; k <= last; k++) {
        sq += r[k] * r[k];
    }
    double corr_rms = sqrt(sq / (double)(2 * n - 1));
    out->peak_to_rms = (corr_rms > 0.0) ? r[p] / corr_rms : 0.0;

    // Second-highest local maximum in range, excluding the main peak's neighbours
    double second = 0.0;
    for (int k = lo; k <= hi; k++) {
        if (k >= p - 1 && k <= p + 1) {
            continue;
        }
        bool left_ok = (k == -last) || r[k] >= r[k - 1];
        bool right_ok = (k == last) || r[k] >= r[k + 1];
        if (left_ok && right_ok && r[k] > second) {
            second = r[k];
        }
    }
    out->second_peak_value = second;

    if (!(r[p] > 0.0)) {
        out->status = DSP_TDOA_NO_PEAK;
        return out->status;
    }

    out->confidence = 1.0 - second / r[p];
    if (out->confidence < 0.0) {
        out->confidence = 0.0;
    }
    if (second >= cfg->ambiguity_ratio * r[p]) {
        out->flags |= DSP_TDOA_FLAG_AMBIGUOUS;
    }
    if (p == lo || p == hi) {
        out->flags |= DSP_TDOA_FLAG_AT_BOUND;
    }

    // Sub-sample refinement (neighbours may be just outside the allowed range)
    double delta = 0.0;
    if (cfg->interpolate && p > -last && p < last) {
        double y0 = r[p - 1], y1 = r[p], y2 = r[p + 1];
        double den = y0 - 2.0 * y1 + y2;
        if (den < 0.0) {
            delta = 0.5 * (y0 - y2) / den;
            if (delta > 0.5) {
                delta = 0.5;
            } else if (delta < -0.5) {
                delta = -0.5;
            }
        }
    }
    out->raw_lag_samples = (double)p + delta;
    out->skew_applied_s = skew;

    // An in-range peak does not prove the signal is physical
    if (g != p && (g < lo || g > hi) && r[g] > r[p]) {
        out->status = DSP_TDOA_PEAK_OUT_OF_RANGE;
        return out->status;
    }
    if (out->peak_to_rms < cfg->min_peak_to_rms) {
        out->status = DSP_TDOA_WEAK_PEAK;
        return out->status;
    }

    double tdoa = out->raw_lag_samples / fs + skew;
    if (fabs(tdoa) > max_tdoa) {
        out->status = DSP_TDOA_TDOA_OUT_OF_RANGE;
        return out->status;
    }

    double x = -cfg->speed_of_sound_mps * tdoa / cfg->mic_spacing_m;
    if (x > 1.0) {
        x = 1.0;
        out->flags |= DSP_TDOA_FLAG_CLAMPED;
    } else if (x < -1.0) {
        x = -1.0;
        out->flags |= DSP_TDOA_FLAG_CLAMPED;
    }

    out->tdoa_s = tdoa;
    out->angle_deg = asin(x) * 180.0 / DSP_PI;
    out->status = DSP_TDOA_OK;
    return out->status;
}

const double *dsp_tdoa_correlation(const struct dsp_tdoa_estimator *est, int *first_lag, size_t *len)
{
    if (est == NULL || est->corr_len == 0) {
        return NULL;
    }
    if (first_lag != NULL) {
        *first_lag = est->corr_first_lag;
    }
    if (len != NULL) {
        *len = est->corr_len;
    }
    return est->corr;
}

const char *dsp_tdoa_status_str(enum dsp_tdoa_status status)
{
    switch (status) {
    case DSP_TDOA_OK:                return "ok";
    case DSP_TDOA_ERR_ARGS:          return "bad arguments";
    case DSP_TDOA_ERR_NONFINITE:     return "non-finite sample";
    case DSP_TDOA_LOW_ENERGY:        return "low energy";
    case DSP_TDOA_NO_PEAK:           return "no correlation peak";
    case DSP_TDOA_PEAK_OUT_OF_RANGE: return "strongest peak outside physical range";
    case DSP_TDOA_TDOA_OUT_OF_RANGE: return "delay outside physical range";
    case DSP_TDOA_WEAK_PEAK:         return "weak correlation peak";
    }
    return "unknown";
}
