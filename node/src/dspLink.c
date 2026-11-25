// dspLink.c
// The real DSP behind dspLink.h: one thread runs
//     AudioCapture -> detector (dsp/detector.h) -> estimator (dsp/tdoa.h)
// and puts each direction it finds in a small queue that the networking
// thread empties with DspLink_takeEvent() (never blocks).
//
// Settings come from environment variables, so the command line stays as it
// is (`node <id> <master IP> [port]`):
//   DSP_MIC_SPACING_M  distance between the two mics in metres (default 0.20; MEASURE IT)
//   DSP_SWAP_MICS      1 = mic A is ADC channel 1 instead of 0 (if theta comes out mirrored)
//   DSP_METHOD         xcorr (default) or phat
//   DSP_BACKEND        auto (default), r5 or linux: which sampler AudioCapture uses
//   DSP_REPLAY         a recording to play instead of the microphones, one frame per
//                      line ("a,b" in ADC counts), looped in real time (no hardware needed)
//   DSP_REPLAY_FS      its sample rate (default 20000)
//   DSP_VERBOSE        1 = print every window and estimate
//
// Directions the estimator refuses (too weak, out of range, ...) are not sent;
// they are counted and printed in the periodic summary. event_id counts every
// detected window, so a gap in event_id at the master means windows that
// were refused, invalid or dropped.
#include "node/dspLink.h"

#include "audio/audio_capture.h"
#include "common/timing.h"
#include "dsp/detector.h"
#include "dsp/tdoa.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MIC_COUNT 2
#define QUEUE_LEN 16
#define READ_TIMEOUT_MS 100
#define RETRY_DELAY_MS 1000           // after the capture stops or fails to start
#define SUMMARY_PERIOD_MS 10000
#define DEFAULT_SPACING_M 0.20
#define DEFAULT_REPLAY_FS 20000.0
#define REPLAY_BLOCK_FRAMES 128
#define NUM_STATUSES (DSP_TDOA_WEAK_PEAK + 1)

struct Settings {
	double spacingM;
	bool swapMics;
	enum dsp_tdoa_method method;
	AudioCapture_Backend backend;
	const char *replayPath;
	double replayFs;
	bool verbose;
};

// Event queue (DSP thread pushes, networking thread pops)
static pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
static struct proto_event queue[QUEUE_LEN];
static int queueHead = 0;
static int queueCount = 0;
static uint32_t dropCount = 0;

static atomic_bool stopRequested;
static atomic_uint_fast64_t blocksProcessed;
static uint64_t blocksAtLastCheck = 0;   // networking thread only
static pthread_t dspThread;
static bool threadStarted = false;
static struct Settings settings;

// Counters for the summary (DSP thread only)
static uint64_t windowsSeen = 0;
static uint64_t windowsInvalid = 0;
static uint64_t estimatesOk = 0;
static uint64_t refusals[NUM_STATUSES];
static uint32_t nextEventId = 0;

// ------------------------------------------------------------ settings

static double envDouble(const char *name, double fallback)
{
	const char *text = getenv(name);
	if (text == NULL || *text == '\0') {
		return fallback;
	}
	char *end = NULL;
	double value = strtod(text, &end);
	if (end == text || *end != '\0') {
		fprintf(stderr, "DSP: ignoring %s=%s (not a number)\n", name, text);
		return fallback;
	}
	return value;
}

static bool envFlag(const char *name)
{
	const char *text = getenv(name);
	return text != NULL && strcmp(text, "1") == 0;
}

static void readSettings(void)
{
	settings.spacingM = envDouble("DSP_MIC_SPACING_M", DEFAULT_SPACING_M);
	settings.swapMics = envFlag("DSP_SWAP_MICS");
	const char *method = getenv("DSP_METHOD");
	settings.method = (method != NULL && strcmp(method, "phat") == 0) ? DSP_TDOA_GCC_PHAT : DSP_TDOA_XCORR;
	const char *backend = getenv("DSP_BACKEND");
	settings.backend = AUDIO_CAPTURE_BACKEND_AUTO;
	if (backend != NULL && strcmp(backend, "r5") == 0) {
		settings.backend = AUDIO_CAPTURE_BACKEND_R5;
	} else if (backend != NULL && strcmp(backend, "linux") == 0) {
		settings.backend = AUDIO_CAPTURE_BACKEND_LINUX;
	}
	settings.replayPath = getenv("DSP_REPLAY");
	settings.replayFs = envDouble("DSP_REPLAY_FS", DEFAULT_REPLAY_FS);
	settings.verbose = envFlag("DSP_VERBOSE");
	if (getenv("DSP_MIC_SPACING_M") == NULL) {
		fprintf(stderr, "DSP: DSP_MIC_SPACING_M not set, assuming %.2f m\n", settings.spacingM);
	}
}

// ------------------------------------------------------------ queue

static void pushEvent(const struct proto_event *event)
{
	pthread_mutex_lock(&queueMutex);
	if (queueCount == QUEUE_LEN) {
		// Nobody took the oldest in time: drop it, keep the newest
		queueHead = (queueHead + 1) % QUEUE_LEN;
		queueCount--;
		dropCount++;
	}
	queue[(queueHead + queueCount) % QUEUE_LEN] = *event;
	queueCount++;
	pthread_mutex_unlock(&queueMutex);
}

// ------------------------------------------------------------ processing

struct Pipeline {
	struct dsp_detector *detector;
	struct dsp_tdoa_estimator *estimator;
	double sampleRateHz;
	double skewS;
};

static bool pipelineCreate(struct Pipeline *p, double sampleRateHz, double skewS)
{
	struct dsp_detector_config dcfg;
	dsp_detector_default_config(&dcfg);
	dcfg.sample_rate_hz = sampleRateHz;
	dcfg.channel_a = settings.swapMics ? 1 : 0;
	dcfg.channel_b = settings.swapMics ? 0 : 1;

	struct dsp_tdoa_config tcfg;
	dsp_tdoa_default_config(&tcfg);
	tcfg.sample_rate_hz = sampleRateHz;
	tcfg.mic_spacing_m = settings.spacingM;
	tcfg.channel_skew_s = skewS;
	tcfg.method = settings.method;

	p->detector = dsp_detector_create(&dcfg);
	size_t windowLen = (size_t)((dcfg.pre_ms + dcfg.post_ms) * 1e-3 * sampleRateHz) + 2;
	p->estimator = dsp_tdoa_create(&tcfg, windowLen);
	p->sampleRateHz = sampleRateHz;
	p->skewS = skewS;
	if (p->detector == NULL || p->estimator == NULL) {
		fprintf(stderr, "DSP: invalid settings (rate %.1f Hz, spacing %.3f m)\n", sampleRateHz,
		        settings.spacingM);
		dsp_detector_destroy(p->detector);
		dsp_tdoa_destroy(p->estimator);
		return false;
	}
	fprintf(stderr, "DSP: %.1f frames/s, spacing %.3f m, skew %.2f us, %s%s\n", sampleRateHz,
	        settings.spacingM, skewS * 1e6, settings.method == DSP_TDOA_GCC_PHAT ? "GCC-PHAT" : "xcorr",
	        settings.swapMics ? ", mics swapped" : "");
	return true;
}

static void pipelineDestroy(struct Pipeline *p)
{
	dsp_detector_destroy(p->detector);
	dsp_tdoa_destroy(p->estimator);
	p->detector = NULL;
	p->estimator = NULL;
}

static void handleWindow(struct Pipeline *p, const struct dsp_detector_window *w)
{
	windowsSeen++;
	uint32_t eventId = nextEventId++;
	if (w->status != DSP_DETECTOR_OK) {
		windowsInvalid++;
		if (settings.verbose) {
			fprintf(stderr, "DSP: window %u invalid: %s\n", eventId, dsp_detector_status_str(w->status));
		}
		return;
	}
	struct dsp_tdoa_input input = dsp_detector_tdoa_input(w);
	struct dsp_tdoa_result result;
	dsp_tdoa_estimate(p->estimator, &input, &result);
	if (settings.verbose) {
		fprintf(stderr, "DSP: window %u: %s, theta %.1f deg, confidence %.2f, peak/rms %.1f, lag %.2f\n",
		        eventId, dsp_tdoa_status_str(result.status), result.angle_deg, result.confidence,
		        result.peak_to_rms, result.raw_lag_samples);
	}
	if (result.status != DSP_TDOA_OK) {
		if ((int)result.status < NUM_STATUSES) {
			refusals[result.status]++;
		}
		return;
	}
	estimatesOk++;
	struct proto_event event = {
		.t_event_ns = (uint64_t)w->t_first_ns,
		.theta = (float)result.angle_deg,
		.confidence = (float)result.confidence,
		.dominant_freq_hz = 0.0f,   // unknown for now (allowed by the protocol)
		.flags = PROTO_EVENT_THETA_MIRROR | (input.clipped ? PROTO_EVENT_CLIPPING : 0),
		.event_id = eventId,
	};
	pushEvent(&event);
}

// Feeds one block (interleaved, 2 channels) through the detector and estimator
static void processBlock(struct Pipeline *p, const uint16_t *frames, int numFrames,
                         const struct dsp_detector_block *block)
{
	dsp_detector_push(p->detector, frames, (size_t)numFrames, block);
	struct dsp_detector_window w;
	while (dsp_detector_next(p->detector, &w)) {
		handleWindow(p, &w);
	}
	atomic_fetch_add(&blocksProcessed, 1);
}

static void printSummary(void)
{
	fprintf(stderr, "DSP: %llu blocks, %llu windows (%llu invalid), %llu directions; refused:",
	        (unsigned long long)atomic_load(&blocksProcessed), (unsigned long long)windowsSeen,
	        (unsigned long long)windowsInvalid, (unsigned long long)estimatesOk);
	bool any = false;
	for (int s = 1; s < NUM_STATUSES; s++) {
		if (refusals[s] > 0) {
			fprintf(stderr, " %s %llu;", dsp_tdoa_status_str((enum dsp_tdoa_status)s),
			        (unsigned long long)refusals[s]);
			any = true;
		}
	}
	fprintf(stderr, "%s dropped %u\n", any ? "" : " none;", DspLink_getDropCount());
}

static void maybePrintSummary(long long *lastMs)
{
	long long now = Timing_getMonotonicMs();
	if (now - *lastMs >= SUMMARY_PERIOD_MS) {
		printSummary();
		*lastMs = now;
	}
}

// ------------------------------------------------------------ sources

// Live microphones through AudioCapture. Returns when stopped or capture ends.
static void runCapture(void)
{
	AudioCapture_Config config = AudioCapture_defaultConfig();
	config.backend = settings.backend;
	if (!AudioCapture_init(&config)) {
		return;
	}
	AudioCapture_Reader *reader = AudioCapture_openReader();
	int channels = AudioCapture_getNumChannels();
	uint16_t *samples = malloc((size_t)AudioCapture_getBlockFrames() * (size_t)channels * sizeof(uint16_t));
	struct Pipeline p = {0};
	int micA = settings.swapMics ? 1 : 0;
	int micB = settings.swapMics ? 0 : 1;
	double skewS = (AudioCapture_getChannelOffsetNs(micB) - AudioCapture_getChannelOffsetNs(micA)) * 1e-9;
	if (reader == NULL || samples == NULL || channels < MIC_COUNT ||
	    !pipelineCreate(&p, AudioCapture_getSampleRateHz(), skewS)) {
		fprintf(stderr, "DSP: unable to start (reader %p, %d channels)\n", (void *)reader, channels);
		free(samples);
		AudioCapture_closeReader(reader);
		AudioCapture_cleanup();
		return;
	}

	long long lastSummary = Timing_getMonotonicMs();
	AudioCapture_BlockInfo info;
	while (!atomic_load(&stopRequested)) {
		AudioCapture_Result result = AudioCapture_readBlock(reader, samples, &info, READ_TIMEOUT_MS);
		if (result == AUDIO_CAPTURE_STOPPED) {
			fprintf(stderr, "DSP: capture stopped\n");
			break;
		}
		if (result == AUDIO_CAPTURE_OK) {
			struct dsp_detector_block block = {
				.t_first_ns = info.tFirstNs,
				.frame_period_ns = info.framePeriodNs,
				.discontinuity = info.blocksSkipped > 0,
				.disturbed = info.timingDisturbed,
			};
			processBlock(&p, samples, info.numFrames, &block);
		}
		maybePrintSummary(&lastSummary);
	}
	pipelineDestroy(&p);
	free(samples);
	AudioCapture_closeReader(reader);
	AudioCapture_cleanup();
}

// A recording ("a,b" per line, ADC counts) played in real time, looped
static uint16_t *loadReplay(const char *path, size_t *numFrames)
{
	FILE *file = fopen(path, "r");
	if (file == NULL) {
		fprintf(stderr, "DSP: unable to open %s: %s\n", path, strerror(errno));
		return NULL;
	}
	size_t cap = 1 << 16, n = 0;
	uint16_t *frames = malloc(cap * MIC_COUNT * sizeof(uint16_t));
	char line[128];
	while (frames != NULL && fgets(line, sizeof(line), file) != NULL) {
		double a, b;
		char *comma = strchr(line, ',');
		if (comma != NULL) {
			*comma = ' ';
		}
		if (line[0] == '#' || sscanf(line, "%lf %lf", &a, &b) != 2) {
			continue;
		}
		if (n == cap) {
			cap *= 2;
			uint16_t *grown = realloc(frames, cap * MIC_COUNT * sizeof(uint16_t));
			if (grown == NULL) {
				break;
			}
			frames = grown;
		}
		frames[MIC_COUNT * n] = (uint16_t)(a < 0 ? 0 : (a > 65535 ? 65535 : a));
		frames[MIC_COUNT * n + 1] = (uint16_t)(b < 0 ? 0 : (b > 65535 ? 65535 : b));
		n++;
	}
	fclose(file);
	*numFrames = n;
	if (frames != NULL && n < REPLAY_BLOCK_FRAMES) {
		fprintf(stderr, "DSP: %s has only %zu frames\n", path, n);
		free(frames);
		return NULL;
	}
	return frames;
}

static void runReplay(void)
{
	size_t total = 0;
	uint16_t *frames = loadReplay(settings.replayPath, &total);
	struct Pipeline p = {0};
	if (frames == NULL || !pipelineCreate(&p, settings.replayFs, 0.0)) {
		free(frames);
		return;
	}
	fprintf(stderr, "DSP: replaying %s (%zu frames, looped)\n", settings.replayPath, total);
	double periodNs = 1e9 / settings.replayFs;
	int64_t startNs = (int64_t)Timing_getRealtimeNs();
	long long startMs = Timing_getMonotonicMs();
	long long lastSummary = startMs;
	uint64_t played = 0;
	size_t pos = 0;
	while (!atomic_load(&stopRequested)) {
		size_t n = REPLAY_BLOCK_FRAMES;
		struct dsp_detector_block block = {
			.t_first_ns = startNs + (int64_t)((double)played * periodNs),
			.frame_period_ns = periodNs,
			.discontinuity = (pos == 0 && played > 0),   // the loop's seam isn't continuous
			.disturbed = false,
		};
		if (pos + n > total) {
			n = total - pos;
		}
		processBlock(&p, frames + MIC_COUNT * pos, (int)n, &block);
		played += n;
		pos = (pos + n) % total;
		// Real time: wait until this audio "has happened"
		long long dueMs = startMs + (long long)((double)played * periodNs / 1e6);
		long long waitMs = dueMs - Timing_getMonotonicMs();
		if (waitMs > 0) {
			Timing_sleepForMs(waitMs);
		}
		maybePrintSummary(&lastSummary);
	}
	pipelineDestroy(&p);
	free(frames);
}

static void *dspLoop(void *arg)
{
	(void)arg;
	while (!atomic_load(&stopRequested)) {
		if (settings.replayPath != NULL) {
			runReplay();
		} else {
			runCapture();
		}
		// Capture ended or never started (for example the R5 was stopped):
		// keep the node up and try again; DspLink_isDspAlive() reports false meanwhile
		for (int waited = 0; waited < RETRY_DELAY_MS && !atomic_load(&stopRequested); waited += 100) {
			Timing_sleepForMs(100);
		}
	}
	printSummary();
	return NULL;
}

// ------------------------------------------------------------ dspLink.h

void DspLink_init(void)
{
	readSettings();
	atomic_store(&stopRequested, false);
	atomic_store(&blocksProcessed, 0);
	int err = pthread_create(&dspThread, NULL, dspLoop, NULL);
	threadStarted = (err == 0);
	if (!threadStarted) {
		fprintf(stderr, "DSP: unable to start the DSP thread: %s\n", strerror(err));
	}
}

void DspLink_cleanup(void)
{
	atomic_store(&stopRequested, true);
	if (threadStarted) {
		pthread_join(dspThread, NULL);
		threadStarted = false;
	}
}

uint8_t DspLink_getMicCount(void)
{
	return MIC_COUNT;
}

bool DspLink_takeEvent(struct proto_event *pEvent)
{
	assert(pEvent != NULL);
	pthread_mutex_lock(&queueMutex);
	bool hasEvent = queueCount > 0;
	if (hasEvent) {
		*pEvent = queue[queueHead];
		queueHead = (queueHead + 1) % QUEUE_LEN;
		queueCount--;
	}
	pthread_mutex_unlock(&queueMutex);
	return hasEvent;
}

bool DspLink_isDspAlive(void)
{
	uint64_t now = atomic_load(&blocksProcessed);
	bool alive = now != blocksAtLastCheck;
	blocksAtLastCheck = now;
	return alive;
}

uint32_t DspLink_getDropCount(void)
{
	pthread_mutex_lock(&queueMutex);
	uint32_t drops = dropCount;
	pthread_mutex_unlock(&queueMutex);
	return drops;
}
