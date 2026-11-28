// audio_capture_r5.h (internal to the audio library)
// R5 backend of AudioCapture: reads the ring the MCU R5F firmware publishes
// in shared memory (r5capture/include/r5cap_shared.h). audio_capture.c
// forwards every public call here while this backend is active; the
// functions mean the same as the public ones in audio/audio_capture.h.
#ifndef AUDIO_CAPTURE_R5_H
#define AUDIO_CAPTURE_R5_H

#include "audio/audio_capture.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// True if the R5 firmware's ring is mapped-able, valid and RUNNING. Used by
// AUTO; quiet on failure (for example when not root).
bool AudioCaptureR5_isSampling(void);

// Maps the ring (and, if possible, the R5's timer) through /dev/mem
bool AudioCaptureR5_init(void);

// For tests: use this memory as the ring region, and read the timer's
// 32-bit counter from timerCounter (NULL: no timer, use block arrival times).
bool AudioCaptureR5_initWithRegion(void *region, size_t regionSize, const volatile uint32_t *timerCounter);

void AudioCaptureR5_cleanup(void);

double AudioCaptureR5_getFramePeriodNs(void);
double AudioCaptureR5_getChannelOffsetNs(int mic);
int AudioCaptureR5_getNumChannels(void);
int AudioCaptureR5_getBlockFrames(void);
uint32_t AudioCaptureR5_getSpiSpeedHz(void);
void AudioCaptureR5_getStats(AudioCapture_Stats *stats);

AudioCapture_Reader *AudioCaptureR5_openReader(void);
void AudioCaptureR5_closeReader(AudioCapture_Reader *reader);
AudioCapture_Result AudioCaptureR5_readBlock(AudioCapture_Reader *reader, uint16_t *samples,
                                             AudioCapture_BlockInfo *info, int timeoutMs);

// True if CLOCK_REALTIME comes from reading the R5's timer directly (precise);
// false if from block arrival times
bool AudioCaptureR5_hasTimerClock(void);

#endif
