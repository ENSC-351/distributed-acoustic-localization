// Streaming impact detector: see dsp/detector.h for the rules it follows.
//
// Frames are processed one at a time, in order. For each frame:
//   1. Store it in the history ring (raw values, and the detection energy).
//   2. Update the energy (moving average of the combined power).
//   3. Run the trigger state machine: ARMED -> (trigger) -> HOLDOFF ->
//      WAIT_REARM -> ARMED, exactly as detector.py's _run() loop.
//   4. Emit any pending window whose last sample has just arrived.
//
// A "segment" is a stretch of frames with no discontinuity. Energy and
// windows never span two segments.
#include "dsp/detector.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PENDING 4          // triggers waiting for their post-trigger samples
#define OUT_SLOTS 8            // finished windows waiting for dsp_detector_next()
#define MIN_PRE_FOR_DC 8       // fewer pre-trigger samples: DC from the whole window

enum trigger_state {
    STATE_ARMED,
    STATE_HOLDOFF,
    STATE_WAIT_REARM,
};

struct out_slot {
    struct dsp_detector_window win;
    float *a, *b, *raw_a, *raw_b;
};

struct dsp_detector {
    struct dsp_detector_config cfg;
    size_t smooth, pre, post, holdoff, len;   // in frames; len = pre + post

    // History ring (power of two, at least len frames)
    size_t ring_size;
    float *hist_a, *hist_b;
    double *hist_energy;      // detection energy per frame (0 while not valid yet)

    // Moving average of the combined power
    double *power;            // last `smooth` powers
    double power_sum;
    size_t energy_count;      // frames in the average so far (valid once == smooth)

    // Noise model and calibration
    bool have_noise;
    struct dsp_detector_noise noise;
    double threshold;
    size_t calib_len;         // frames to collect
    size_t calib_count;
    float *calib_a, *calib_b;
    double *scratch;          // medians (max(calib_len, len) values)

    // Position and timing
    uint64_t pos;             // frames processed so far (all segments)
    uint64_t seg_start;       // first frame of the current segment
    bool ever_started;        // a frame has been processed at all
    uint64_t anchor_frame;    // first frame of the latest block in this segment
    int64_t anchor_ns;
    double period_ns;

    // Trigger state machine
    enum trigger_state state;
    uint64_t last_trigger;
    uint64_t pending[MAX_PENDING];
    size_t num_pending;

    // Output queue
    struct out_slot slots[OUT_SLOTS];
    size_t out_head, out_count;   // oldest waiting slot, number waiting
    bool taken_valid;             // a slot was handed out and is still in use
    size_t taken;
    uint32_t dropped;
};

// ------------------------------------------------------------------ helpers

static size_t ms_to_frames(double ms, double fs)
{
    return (size_t)llround(ms * 1e-3 * fs);
}

static int cmp_double(const void *x, const void *y)
{
    double a = *(const double *)x, b = *(const double *)y;
    return (a > b) - (a < b);
}

// Median like numpy: average of the two middle values for an even count.
// Sorts `buf` in place.
static double median(double *buf, size_t n)
{
    qsort(buf, n, sizeof(double), cmp_double);
    if (n % 2 == 1) {
        return buf[n / 2];
    }
    return 0.5 * (buf[n / 2 - 1] + buf[n / 2]);
}

static size_t ring_index(const struct dsp_detector *det, uint64_t frame)
{
    return (size_t)(frame & (det->ring_size - 1));
}

static void reset_energy(struct dsp_detector *det)
{
    det->power_sum = 0.0;
    det->energy_count = 0;
}

static void set_noise_internal(struct dsp_detector *det, const struct dsp_detector_noise *noise)
{
    det->noise = *noise;
    if (!(det->noise.floor_energy >= det->cfg.min_noise_floor)) {   // also catches NaN
        det->noise.floor_energy = det->cfg.min_noise_floor;
    }
    det->threshold = det->cfg.threshold_ratio * det->noise.floor_energy;
    det->have_noise = true;
    // Energies so far used another DC (or none): start the average again
    reset_energy(det);
}

// Calibration done: DC = median of each channel, floor = median of the valid
// energies computed with that DC (as calibrate_noise() in detector.py).
static void finish_calibration(struct dsp_detector *det)
{
    size_t n = det->calib_count;
    struct dsp_detector_noise noise;
    for (size_t i = 0; i < n; i++) {
        det->scratch[i] = det->calib_a[i];
    }
    noise.dc_a = median(det->scratch, n);
    for (size_t i = 0; i < n; i++) {
        det->scratch[i] = det->calib_b[i];
    }
    noise.dc_b = median(det->scratch, n);

    // Causal moving average; the first smooth-1 values have too little history
    double sum = 0.0;
    size_t valid = 0;
    for (size_t i = 0; i < n; i++) {
        double da = det->calib_a[i] - noise.dc_a;
        double db = det->calib_b[i] - noise.dc_b;
        sum += da * da + db * db;
        if (i >= det->smooth) {
            double oa = det->calib_a[i - det->smooth] - noise.dc_a;
            double ob = det->calib_b[i - det->smooth] - noise.dc_b;
            sum -= oa * oa + ob * ob;
        }
        if (i + 1 >= det->smooth) {
            det->scratch[valid++] = sum / (double)det->smooth;
        }
    }
    noise.floor_energy = (valid > 0) ? median(det->scratch, valid) : 0.0;
    set_noise_internal(det, &noise);
}

// Time of a frame in the current segment, from the latest block's timing
static int64_t frame_time_ns(const struct dsp_detector *det, uint64_t frame)
{
    double offset = ((double)frame - (double)det->anchor_frame) * det->period_ns;
    return det->anchor_ns + (int64_t)llround(offset);
}

// Claims an output slot, dropping the oldest waiting window if all are full.
// Only called during a push, which first releases the slot the caller was
// reading, so every slot not waiting in the queue is free.
static struct out_slot *claim_slot(struct dsp_detector *det)
{
    if (det->out_count == OUT_SLOTS) {
        det->out_head = (det->out_head + 1) % OUT_SLOTS;
        det->out_count--;
        det->dropped++;
    }
    size_t idx = (det->out_head + det->out_count) % OUT_SLOTS;
    det->out_count++;
    return &det->slots[idx];
}

static void release_taken(struct dsp_detector *det)
{
    det->taken_valid = false;
}

// An invalid window (no samples) for a trigger at `trigger`
static void emit_invalid(struct dsp_detector *det, uint64_t trigger, enum dsp_detector_status status)
{
    struct out_slot *slot = claim_slot(det);
    struct dsp_detector_window *w = &slot->win;
    memset(w, 0, sizeof(*w));
    w->status = status;
    // start_frame + trigger_index is always the trigger frame
    w->start_frame = (trigger >= det->pre) ? trigger - det->pre : 0;
    w->trigger_index = (size_t)(trigger - w->start_frame);
    w->t_first_ns = det->ever_started ? frame_time_ns(det, w->start_frame) : 0;
    w->dc_a = NAN;
    w->dc_b = NAN;
    w->threshold_energy = det->threshold;
    w->peak_to_floor = NAN;
}

// Copies a complete window out of the history ring
static void emit_window(struct dsp_detector *det, uint64_t trigger)
{
    uint64_t start = trigger - det->pre;
    struct out_slot *slot = claim_slot(det);
    struct dsp_detector_window *w = &slot->win;
    memset(w, 0, sizeof(*w));

    double peak_energy = 0.0;
    for (size_t i = 0; i < det->len; i++) {
        size_t r = ring_index(det, start + i);
        slot->raw_a[i] = det->hist_a[r];
        slot->raw_b[i] = det->hist_b[r];
        if (det->hist_energy[r] > peak_energy) {
            peak_energy = det->hist_energy[r];
        }
    }

    // DC from the pre-trigger samples (no impact yet), or the whole window
    size_t dc_len = det->pre;
    if (det->pre < MIN_PRE_FOR_DC) {
        dc_len = det->len;
        w->flags |= DSP_DETECTOR_FLAG_SHORT_PRE;
    }
    for (size_t i = 0; i < dc_len; i++) {
        det->scratch[i] = slot->raw_a[i];
    }
    w->dc_a = median(det->scratch, dc_len);
    for (size_t i = 0; i < dc_len; i++) {
        det->scratch[i] = slot->raw_b[i];
    }
    w->dc_b = median(det->scratch, dc_len);

    for (size_t i = 0; i < det->len; i++) {
        slot->a[i] = (float)(slot->raw_a[i] - w->dc_a);
        slot->b[i] = (float)(slot->raw_b[i] - w->dc_b);
        if (det->cfg.check_clipping) {
            if (slot->raw_a[i] <= det->cfg.adc_min || slot->raw_a[i] >= det->cfg.adc_max) {
                w->flags |= DSP_DETECTOR_FLAG_CLIPPED_A;
            }
            if (slot->raw_b[i] <= det->cfg.adc_min || slot->raw_b[i] >= det->cfg.adc_max) {
                w->flags |= DSP_DETECTOR_FLAG_CLIPPED_B;
            }
        }
    }

    w->status = DSP_DETECTOR_OK;
    w->a = slot->a;
    w->b = slot->b;
    w->raw_a = slot->raw_a;
    w->raw_b = slot->raw_b;
    w->n = det->len;
    w->trigger_index = det->pre;
    w->start_frame = start;
    w->t_first_ns = frame_time_ns(det, start);
    w->threshold_energy = det->threshold;
    w->peak_to_floor = peak_energy / det->noise.floor_energy;
}

// A discontinuity: windows still waiting for samples can't be completed
static void start_segment(struct dsp_detector *det)
{
    for (size_t i = 0; i < det->num_pending; i++) {
        emit_invalid(det, det->pending[i], DSP_DETECTOR_GAP);
    }
    det->num_pending = 0;
    det->seg_start = det->pos;
    reset_energy(det);
    if (!det->have_noise) {
        det->calib_count = 0;   // calibration must be one continuous stretch
    }
}

static void process_frame(struct dsp_detector *det, double a, double b)
{
    uint64_t i = det->pos;
    size_t r = ring_index(det, i);
    det->hist_a[r] = (float)a;
    det->hist_b[r] = (float)b;
    det->pos++;
    det->ever_started = true;

    if (!det->have_noise) {
        det->hist_energy[r] = 0.0;
        det->calib_a[det->calib_count] = (float)a;
        det->calib_b[det->calib_count] = (float)b;
        det->calib_count++;
        if (det->calib_count == det->calib_len) {
            finish_calibration(det);
        }
        return;
    }

    // Energy: causal moving average over `smooth` frames of the combined power
    double da = a - det->noise.dc_a;
    double db = b - det->noise.dc_b;
    double p = da * da + db * db;
    size_t pr = (size_t)(i % det->smooth);
    if (det->energy_count == det->smooth) {
        det->power_sum -= det->power[pr];
    } else {
        det->energy_count++;
    }
    det->power[pr] = p;
    det->power_sum += p;
    // Not enough history yet counts as 0 (detector.py: NaN -> 0)
    double e = (det->energy_count == det->smooth) ? det->power_sum / (double)det->smooth : 0.0;
    if (e < 0.0) {
        e = 0.0;   // rounding in the running sum
    }
    det->hist_energy[r] = e;

    // Trigger state machine (same order of checks as detector.py)
    if (det->state == STATE_HOLDOFF && i >= det->last_trigger + det->holdoff) {
        det->state = STATE_WAIT_REARM;
    }
    if (det->state == STATE_WAIT_REARM && e <= det->cfg.rearm_ratio * det->threshold) {
        det->state = STATE_ARMED;
    } else if (det->state == STATE_ARMED && e > det->threshold) {
        det->last_trigger = i;
        det->state = (det->holdoff > 0) ? STATE_HOLDOFF : STATE_WAIT_REARM;
        if (i < det->seg_start + det->pre) {
            // Not enough history before the trigger in this segment
            emit_invalid(det, i, (det->seg_start == 0) ? DSP_DETECTOR_INCOMPLETE : DSP_DETECTOR_GAP);
        } else if (det->num_pending < MAX_PENDING) {
            det->pending[det->num_pending++] = i;
        } else {
            det->dropped++;
        }
    }

    // Windows whose last sample was this frame
    size_t kept = 0;
    for (size_t k = 0; k < det->num_pending; k++) {
        if (det->pending[k] + det->post == det->pos) {
            emit_window(det, det->pending[k]);
        } else {
            det->pending[kept++] = det->pending[k];
        }
    }
    det->num_pending = kept;
}

static bool begin_block(struct dsp_detector *det, size_t num_frames, const struct dsp_detector_block *block)
{
    release_taken(det);
    if (num_frames == 0 || block == NULL || !(block->frame_period_ns > 0.0)) {
        return false;
    }
    if (block->discontinuity && det->ever_started) {
        start_segment(det);
    }
    det->anchor_frame = det->pos;
    det->anchor_ns = block->t_first_ns;
    det->period_ns = block->frame_period_ns;
    return true;
}

// A block with untrustworthy timing: its frames are counted but not used,
// and it ends the segment
static void skip_disturbed(struct dsp_detector *det, size_t num_frames)
{
    det->ever_started = true;
    det->pos += num_frames;
    start_segment(det);
}

// ------------------------------------------------------------------ API

void dsp_detector_default_config(struct dsp_detector_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->sample_rate_hz = 0.0;
    cfg->num_channels = 2;
    cfg->channel_a = 0;
    cfg->channel_b = 1;
    cfg->threshold_ratio = 12.0;
    cfg->smooth_ms = 0.5;
    cfg->pre_ms = 5.0;
    cfg->post_ms = 15.0;
    cfg->holdoff_ms = 50.0;
    cfg->rearm_ratio = 0.5;
    cfg->min_noise_floor = 1e-9;
    cfg->check_clipping = true;
    cfg->adc_min = 0.0;
    cfg->adc_max = 4095.0;
    cfg->calibrate_ms = 1000.0;
}

bool dsp_detector_config_valid(const struct dsp_detector_config *cfg)
{
    if (cfg == NULL) {
        return false;
    }
    double fs = cfg->sample_rate_hz;
    bool ok = isfinite(fs) && fs > 0.0 && fs < 1e7 &&
              cfg->num_channels >= 1 && cfg->num_channels <= 64 &&
              cfg->channel_a >= 0 && cfg->channel_a < cfg->num_channels &&
              cfg->channel_b >= 0 && cfg->channel_b < cfg->num_channels &&
              isfinite(cfg->threshold_ratio) && cfg->threshold_ratio > 0.0 &&
              isfinite(cfg->smooth_ms) && cfg->smooth_ms >= 0.0 &&
              isfinite(cfg->pre_ms) && cfg->pre_ms >= 0.0 &&
              isfinite(cfg->post_ms) && cfg->post_ms > 0.0 &&
              isfinite(cfg->holdoff_ms) && cfg->holdoff_ms >= 0.0 &&
              isfinite(cfg->rearm_ratio) && cfg->rearm_ratio > 0.0 && cfg->rearm_ratio <= 1.0 &&
              isfinite(cfg->min_noise_floor) && cfg->min_noise_floor > 0.0 &&
              isfinite(cfg->calibrate_ms) && cfg->calibrate_ms >= 0.0 &&
              (!cfg->check_clipping || (isfinite(cfg->adc_min) && isfinite(cfg->adc_max) &&
                                        cfg->adc_min < cfg->adc_max));
    if (!ok) {
        return false;
    }
    // Keep the buffers to a sane size (60 s of calibration, 10 s windows)
    return cfg->calibrate_ms * 1e-3 * fs <= 60.0 * fs &&
           (cfg->pre_ms + cfg->post_ms) <= 10000.0;
}

struct dsp_detector *dsp_detector_create(const struct dsp_detector_config *cfg)
{
    if (!dsp_detector_config_valid(cfg)) {
        return NULL;
    }
    struct dsp_detector *det = calloc(1, sizeof(*det));
    if (det == NULL) {
        return NULL;
    }
    double fs = cfg->sample_rate_hz;
    det->cfg = *cfg;
    det->smooth = ms_to_frames(cfg->smooth_ms, fs);
    if (det->smooth < 1) {
        det->smooth = 1;
    }
    det->pre = ms_to_frames(cfg->pre_ms, fs);
    det->post = ms_to_frames(cfg->post_ms, fs);
    if (det->post < 1) {
        det->post = 1;
    }
    det->holdoff = ms_to_frames(cfg->holdoff_ms, fs);
    det->len = det->pre + det->post;
    det->calib_len = ms_to_frames(cfg->calibrate_ms, fs);
    if (det->calib_len < det->smooth) {
        det->calib_len = det->smooth;   // need at least one valid energy
    }

    det->ring_size = 1;
    while (det->ring_size < det->len + 1) {
        det->ring_size <<= 1;
    }
    size_t scratch_len = (det->calib_len > det->len) ? det->calib_len : det->len;

    bool ok = true;
    ok &= (det->hist_a = calloc(det->ring_size, sizeof(float))) != NULL;
    ok &= (det->hist_b = calloc(det->ring_size, sizeof(float))) != NULL;
    ok &= (det->hist_energy = calloc(det->ring_size, sizeof(double))) != NULL;
    ok &= (det->power = calloc(det->smooth, sizeof(double))) != NULL;
    ok &= (det->calib_a = calloc(det->calib_len, sizeof(float))) != NULL;
    ok &= (det->calib_b = calloc(det->calib_len, sizeof(float))) != NULL;
    ok &= (det->scratch = calloc(scratch_len, sizeof(double))) != NULL;
    for (size_t s = 0; s < OUT_SLOTS; s++) {
        ok &= (det->slots[s].a = calloc(det->len, sizeof(float))) != NULL;
        ok &= (det->slots[s].b = calloc(det->len, sizeof(float))) != NULL;
        ok &= (det->slots[s].raw_a = calloc(det->len, sizeof(float))) != NULL;
        ok &= (det->slots[s].raw_b = calloc(det->len, sizeof(float))) != NULL;
    }
    if (!ok) {
        dsp_detector_destroy(det);
        return NULL;
    }
    det->state = STATE_ARMED;
    det->period_ns = 1e9 / fs;
    return det;
}

void dsp_detector_destroy(struct dsp_detector *det)
{
    if (det == NULL) {
        return;
    }
    free(det->hist_a);
    free(det->hist_b);
    free(det->hist_energy);
    free(det->power);
    free(det->calib_a);
    free(det->calib_b);
    free(det->scratch);
    for (size_t s = 0; s < OUT_SLOTS; s++) {
        free(det->slots[s].a);
        free(det->slots[s].b);
        free(det->slots[s].raw_a);
        free(det->slots[s].raw_b);
    }
    free(det);
}

void dsp_detector_set_noise(struct dsp_detector *det, const struct dsp_detector_noise *noise)
{
    if (det != NULL && noise != NULL) {
        set_noise_internal(det, noise);
    }
}

bool dsp_detector_get_noise(const struct dsp_detector *det, struct dsp_detector_noise *noise)
{
    if (det == NULL || !det->have_noise) {
        return false;
    }
    if (noise != NULL) {
        *noise = det->noise;
    }
    return true;
}

size_t dsp_detector_push(struct dsp_detector *det, const uint16_t *frames, size_t num_frames,
                         const struct dsp_detector_block *block)
{
    if (det == NULL || frames == NULL || !begin_block(det, num_frames, block)) {
        return det ? det->out_count : 0;
    }
    if (block->disturbed) {
        skip_disturbed(det, num_frames);
        return det->out_count;
    }
    size_t nc = (size_t)det->cfg.num_channels;
    for (size_t f = 0; f < num_frames; f++) {
        const uint16_t *frame = frames + f * nc;
        process_frame(det, frame[det->cfg.channel_a], frame[det->cfg.channel_b]);
    }
    return det->out_count;
}

size_t dsp_detector_push_float(struct dsp_detector *det, const float *frames, size_t num_frames,
                               const struct dsp_detector_block *block)
{
    if (det == NULL || frames == NULL || !begin_block(det, num_frames, block)) {
        return det ? det->out_count : 0;
    }
    if (block->disturbed) {
        skip_disturbed(det, num_frames);
        return det->out_count;
    }
    size_t nc = (size_t)det->cfg.num_channels;
    for (size_t f = 0; f < num_frames; f++) {
        const float *frame = frames + f * nc;
        process_frame(det, frame[det->cfg.channel_a], frame[det->cfg.channel_b]);
    }
    return det->out_count;
}

bool dsp_detector_next(struct dsp_detector *det, struct dsp_detector_window *out)
{
    if (det == NULL) {
        return false;
    }
    release_taken(det);
    if (det->out_count == 0) {
        return false;
    }
    size_t idx = det->out_head;
    det->out_head = (det->out_head + 1) % OUT_SLOTS;
    det->out_count--;
    det->taken = idx;
    det->taken_valid = true;
    if (out != NULL) {
        *out = det->slots[idx].win;
    }
    return true;
}

uint32_t dsp_detector_dropped(const struct dsp_detector *det)
{
    return det ? det->dropped : 0;
}

void dsp_detector_reset(struct dsp_detector *det)
{
    if (det == NULL) {
        return;
    }
    det->num_pending = 0;
    det->out_head = 0;
    det->out_count = 0;
    det->taken_valid = false;
    det->state = STATE_ARMED;
    reset_energy(det);
    det->seg_start = det->pos;
    if (!det->have_noise) {
        det->calib_count = 0;
    }
}

const char *dsp_detector_status_str(enum dsp_detector_status status)
{
    switch (status) {
    case DSP_DETECTOR_OK:
        return "ok";
    case DSP_DETECTOR_INCOMPLETE:
        return "incomplete (not enough history before the trigger)";
    case DSP_DETECTOR_GAP:
        return "gap (a discontinuity falls inside the window)";
    }
    return "unknown";
}
