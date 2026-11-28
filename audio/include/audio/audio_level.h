/**
 * @file audio_level.h
 * @brief Measures the loudness of each microphone from the AudioCapture stream.
 *
 * Usage:
 *     AudioCapture_init(NULL);              // start sampling first
 *     AudioLevel_init(NULL);                // starts its own reader thread
 *     double level = AudioLevel_getLevel(0); // mic 0: 0.0 (silent) .. 1.0 (full scale)
 *     AudioLevel_cleanup();                 // before AudioCapture_cleanup()
 *
 * Microphones are numbered by their position in AudioCapture_Config.channels
 * (mic 0, mic 1, ...), not by ADC channel.
 *
 * The getters are thread-safe and non-blocking, so any thread (display,
 * LED, network, ...) can poll them at whatever rate it likes.
 *
 * How it works, for each microphone:
 *   - Tracks the DC bias (mic idles around mid-scale, ~2048) with a slow
 *     running average and subtracts it.
 *   - Every windowMs, takes the peak deviation in that window as the
 *     amplitude.
 *   - The level jumps up instantly and falls back over releaseMs, like a
 *     VU meter, so it doesn't flicker.
 */
#ifndef AUDIO_LEVEL_H
#define AUDIO_LEVEL_H

#include <stdbool.h>

typedef struct {
    int windowMs;         // Amplitude measurement window
    int fullScaleCounts;  // Amplitude (ADC counts) that maps to level 1.0;
                          //   lower this if your mic is quiet and the bar barely moves
    int releaseMs;        // Time for the level to fall from 1.0 back to 0.0
} AudioLevel_Config;

/**
 * @brief Returns a config filled with sensible defaults.
 */
AudioLevel_Config AudioLevel_defaultConfig(void);

/**
 * @brief Starts measuring. AudioCapture must already be running.
 * @param config Settings to use, or NULL for AudioLevel_defaultConfig().
 * @return true on success, false on bad config, capture not running, or thread failure.
 */
bool AudioLevel_init(const AudioLevel_Config *config);

/**
 * @brief Stops measuring. Call before AudioCapture_cleanup().
 */
void AudioLevel_cleanup(void);

/**
 * @brief Number of microphones being measured (same as AudioCapture).
 */
int AudioLevel_getNumMics(void);

/**
 * @brief Smoothed loudness for display, 0.0 (silent) to 1.0 (full scale).
 *        Returns 0.0 for an invalid mic.
 */
double AudioLevel_getLevel(int mic);

/**
 * @brief Unsmoothed peak amplitude of the most recent window, in ADC counts
 *        (0 to about 2048).
 */
int AudioLevel_getAmplitude(int mic);

/**
 * @brief Most recent raw ADC reading (0-4095), for checking the wiring.
 */
int AudioLevel_getLastSample(int mic);

#endif
