#include "audio/audio_level.h"
#include "audio/audio_capture.h"
#include "hal/adc.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_WINDOW_MS 20
#define DEFAULT_FULL_SCALE_COUNTS 2048
#define DEFAULT_RELEASE_MS 500

// How quickly the DC-bias estimate follows drift. Much longer than an
// audio period so it doesn't cancel out the sound itself.
#define DC_TRACKING_TIME_CONSTANT_S 0.5

// How long the reader thread waits for a block before checking for shutdown
#define READ_TIMEOUT_MS 100

#define MS_PER_S 1000

static AudioLevel_Config config;
static bool isRunning = false;
static pthread_t levelThread;
static atomic_bool stopRequested;
static AudioCapture_Reader *reader = NULL;
static uint16_t *blockBuffer = NULL;
static int numMics = 0;

// Shared between the level thread and getters
static pthread_mutex_t dataMutex = PTHREAD_MUTEX_INITIALIZER;
static double smoothedLevel[AUDIO_CAPTURE_MAX_CHANNELS];
static int windowAmplitude[AUDIO_CAPTURE_MAX_CHANNELS];
static int lastSample[AUDIO_CAPTURE_MAX_CHANNELS];

static void *levelLoop(void *arg)
{
    (void)arg;

    const double sampleRateHz = AudioCapture_getSampleRateHz();
    int samplesPerWindow = (int)(sampleRateHz * config.windowMs / MS_PER_S);
    if (samplesPerWindow < 1) {
        samplesPerWindow = 1;
    }
    const double dcAlpha = 1.0 / (DC_TRACKING_TIME_CONSTANT_S * sampleRateHz);
    const double decayPerWindow = (double)config.windowMs / config.releaseMs;

    double dcOffset[AUDIO_CAPTURE_MAX_CHANNELS];
    int windowPeak[AUDIO_CAPTURE_MAX_CHANNELS] = { 0 };
    double level[AUDIO_CAPTURE_MAX_CHANNELS] = { 0 };
    bool dcSeeded = false;
    int samplesInWindow = 0;

    while (!atomic_load(&stopRequested)) {
        AudioCapture_BlockInfo info;
        AudioCapture_Result result = AudioCapture_readBlock(reader, blockBuffer, &info, READ_TIMEOUT_MS);
        if (result == AUDIO_CAPTURE_STOPPED) {
            break;
        }
        if (result != AUDIO_CAPTURE_OK) {
            continue;
        }

        if (!dcSeeded) {
            for (int mic = 0; mic < numMics; mic++) {
                dcOffset[mic] = blockBuffer[mic];
            }
            dcSeeded = true;
        }

        for (int frame = 0; frame < info.numFrames; frame++) {
            const uint16_t *samples = blockBuffer + (size_t)frame * (size_t)info.numChannels;
            for (int mic = 0; mic < numMics; mic++) {
                dcOffset[mic] += (samples[mic] - dcOffset[mic]) * dcAlpha;
                int deviation = abs((int)(samples[mic] - dcOffset[mic]));
                if (deviation > windowPeak[mic]) {
                    windowPeak[mic] = deviation;
                }
            }
            samplesInWindow++;

            if (samplesInWindow >= samplesPerWindow) {
                for (int mic = 0; mic < numMics; mic++) {
                    double instant = (double)windowPeak[mic] / config.fullScaleCounts;
                    if (instant > 1.0) {
                        instant = 1.0;
                    }
                    // Fast attack, slow release
                    if (instant >= level[mic]) {
                        level[mic] = instant;
                    } else {
                        level[mic] -= decayPerWindow;
                        if (level[mic] < instant) {
                            level[mic] = instant;
                        }
                    }
                }

                pthread_mutex_lock(&dataMutex);
                for (int mic = 0; mic < numMics; mic++) {
                    smoothedLevel[mic] = level[mic];
                    windowAmplitude[mic] = windowPeak[mic];
                }
                pthread_mutex_unlock(&dataMutex);

                memset(windowPeak, 0, sizeof(windowPeak));
                samplesInWindow = 0;
            }
        }

        const uint16_t *lastFrame = blockBuffer + (size_t)(info.numFrames - 1) * (size_t)info.numChannels;
        pthread_mutex_lock(&dataMutex);
        for (int mic = 0; mic < numMics; mic++) {
            lastSample[mic] = lastFrame[mic];
        }
        pthread_mutex_unlock(&dataMutex);
    }
    return NULL;
}

AudioLevel_Config AudioLevel_defaultConfig(void)
{
    AudioLevel_Config defaults = {
        .windowMs = DEFAULT_WINDOW_MS,
        .fullScaleCounts = DEFAULT_FULL_SCALE_COUNTS,
        .releaseMs = DEFAULT_RELEASE_MS,
    };
    return defaults;
}

bool AudioLevel_init(const AudioLevel_Config *userConfig)
{
    if (isRunning) {
        fprintf(stderr, "AudioLevel: already initialized\n");
        return false;
    }

    config = userConfig ? *userConfig : AudioLevel_defaultConfig();
    if (config.windowMs <= 0 || config.fullScaleCounts <= 0 || config.releaseMs <= 0) {
        fprintf(stderr, "AudioLevel: invalid configuration\n");
        return false;
    }

    reader = AudioCapture_openReader();
    if (reader == NULL) {
        fprintf(stderr, "AudioLevel: AudioCapture must be running first\n");
        return false;
    }
    numMics = AudioCapture_getNumChannels();
    blockBuffer = malloc((size_t)AudioCapture_getBlockFrames() * (size_t)numMics * sizeof(uint16_t));
    if (blockBuffer == NULL) {
        fprintf(stderr, "AudioLevel: out of memory\n");
        AudioCapture_closeReader(reader);
        reader = NULL;
        return false;
    }

    pthread_mutex_lock(&dataMutex);
    for (int mic = 0; mic < AUDIO_CAPTURE_MAX_CHANNELS; mic++) {
        smoothedLevel[mic] = 0.0;
        windowAmplitude[mic] = 0;
        lastSample[mic] = ADC_MIDPOINT;
    }
    pthread_mutex_unlock(&dataMutex);

    atomic_store(&stopRequested, false);
    if (pthread_create(&levelThread, NULL, levelLoop, NULL) != 0) {
        perror("AudioLevel: unable to create thread");
        free(blockBuffer);
        blockBuffer = NULL;
        AudioCapture_closeReader(reader);
        reader = NULL;
        return false;
    }
    isRunning = true;
    return true;
}

void AudioLevel_cleanup(void)
{
    if (!isRunning) {
        return;
    }
    atomic_store(&stopRequested, true);
    pthread_join(levelThread, NULL);
    AudioCapture_closeReader(reader);
    reader = NULL;
    free(blockBuffer);
    blockBuffer = NULL;
    isRunning = false;
}

int AudioLevel_getNumMics(void)
{
    return numMics;
}

static bool isValidMic(int mic)
{
    return mic >= 0 && mic < numMics;
}

double AudioLevel_getLevel(int mic)
{
    if (!isValidMic(mic)) {
        return 0.0;
    }
    pthread_mutex_lock(&dataMutex);
    double value = smoothedLevel[mic];
    pthread_mutex_unlock(&dataMutex);
    return value;
}

int AudioLevel_getAmplitude(int mic)
{
    if (!isValidMic(mic)) {
        return 0;
    }
    pthread_mutex_lock(&dataMutex);
    int value = windowAmplitude[mic];
    pthread_mutex_unlock(&dataMutex);
    return value;
}

int AudioLevel_getLastSample(int mic)
{
    if (!isValidMic(mic)) {
        return 0;
    }
    pthread_mutex_lock(&dataMutex);
    int value = lastSample[mic];
    pthread_mutex_unlock(&dataMutex);
    return value;
}
