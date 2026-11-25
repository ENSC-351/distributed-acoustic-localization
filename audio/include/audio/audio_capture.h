/**
 * @file audio_capture.h
 * @brief Samples the microphones as fast as the ADC allows and hands out the
 *        raw samples in timestamped blocks, for signal analysis.
 *
 * This is the Linux reserved-core version (see audio/CONTEXT.md). It needs
 * hardware SPI and a CPU core reserved with isolcpus= (see overlays/README.md).
 *
 * One background thread reads each block of samples in a single SPI message,
 * so the SPI hardware, not software timing, sets the spacing between samples.
 * The sample rate is therefore set by the SPI clock rather than chosen
 * directly: it is measured when capture starts (AudioCapture_getSampleRateHz()).
 *
 * Any number of readers (for example the localization code, the level meter,
 * and a recorder) can each receive every block independently. The sampling
 * thread never waits for a reader; a reader that falls too far behind skips
 * ahead, and is told so.
 *
 * Usage:
 *     AudioCapture_init(NULL);                       // defaults: ADC ch 0 and 1
 *     AudioCapture_Reader *reader = AudioCapture_openReader();
 *     uint16_t *samples = malloc(AudioCapture_getBlockFrames() *
 *                                AudioCapture_getNumChannels() * sizeof(uint16_t));
 *     AudioCapture_BlockInfo info;
 *     while (AudioCapture_readBlock(reader, samples, &info, -1) == AUDIO_CAPTURE_OK) {
 *         for (int f = 0; f < info.numFrames; f++) {
 *             uint16_t mic0 = samples[f * info.numChannels + 0];
 *             uint16_t mic1 = samples[f * info.numChannels + 1];
 *             double tNs = info.tFirstNs + f * info.framePeriodNs;   // time of mic0's sample
 *         }
 *     }
 *     AudioCapture_closeReader(reader);
 *     AudioCapture_cleanup();
 *
 * Sample format: raw 12-bit ADC counts, 0 to ADC_MAX_READING. The microphone
 * idles near ADC_MIDPOINT (2048); subtract the actual DC offset (for example
 * the block's mean) before analysis.
 *
 * Timing:
 *   - Inside a block, every conversion takes the same time, so frames are
 *     evenly spaced framePeriodNs apart with no gaps. A block can go straight
 *     into an FFT or cross-correlation. framePeriodNs is measured for each
 *     block; blocks whose timing was disturbed are flagged (timingDisturbed).
 *   - Within a frame, the channels are read one after another, evenly spaced:
 *     channel k is sampled k * framePeriodNs / numChannels after channel 0
 *     (AudioCapture_getChannelOffsetNs()). Subtract this when comparing arrival
 *     times between microphones.
 *   - Between blocks there is a short gap (the system-call overhead, roughly
 *     tens of microseconds): see AudioCapture_BlockInfo.gapBeforeNs.
 *   - tFirstNs is the CLOCK_REALTIME time of the block's first sample, so
 *     blocks from different boards can be compared once their clocks are
 *     synced. It is estimated from the time around the SPI call, so it is only
 *     accurate to about half the call overhead; times within a block are much
 *     more precise.
 */
#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

// Most microphones one capture can sample
#define AUDIO_CAPTURE_MAX_CHANNELS 8

// AudioCapture_getReservedCpu() when no core is reserved
#define AUDIO_CAPTURE_NO_CPU (-1)

// Fewest frames per block (needed to measure the sample rate)
#define AUDIO_CAPTURE_MIN_BLOCK_FRAMES 8

typedef struct {
    int numChannels;                                // Microphones to sample (1 to AUDIO_CAPTURE_MAX_CHANNELS)
    uint8_t channels[AUDIO_CAPTURE_MAX_CHANNELS];   // ADC channel of each microphone, in frame order
    int blockFrames;                                // Frames per block (one SPI message). blockFrames * numChannels
                                                    //   may not exceed ADC_MAX_CONVERSIONS_PER_READ.
    uint32_t spiSpeedHz;                            // SPI clock; sets the sample rate. See ADC_DEFAULT_SPI_SPEED_HZ.
    int ringBlocks;                                 // Blocks kept in memory; a reader further behind than this loses data
} AudioCapture_Config;

typedef struct {
    uint64_t blockNumber;    // Counts blocks from the start of capture
    int64_t tFirstNs;        // CLOCK_REALTIME time (ns) of the first sample (channel 0 of frame 0)
    double framePeriodNs;    // Time between frames inside the block, measured for this block
    int numFrames;           // Frames in this block (always the configured blockFrames)
    int numChannels;         // Samples per frame
    uint64_t blocksSkipped;  // Blocks this reader missed (fell too far behind) since its previous block
    int64_t gapBeforeNs;     // Estimated time from the end of this reader's previous block to the start
                             //   of this one; 0 for a reader's first block. Small unless blocks were skipped.
    bool timingDisturbed;    // The block took noticeably longer or shorter than usual (for example an
                             //   interrupt paused the SPI transfer partway), so its samples may not be
                             //   evenly spaced. Consider skipping it for timing-sensitive analysis.
} AudioCapture_BlockInfo;

typedef struct {
    uint64_t blocksCaptured; // Blocks delivered
    uint64_t framesCaptured; // Frames delivered
    uint64_t adcErrors;      // Failed SPI messages (each loses one block)
    uint64_t blocksDisturbed; // Blocks flagged timingDisturbed
    int64_t maxGapNs;        // Longest estimated gap between consecutive blocks
    double callOverheadNs;   // Measured time each SPI message adds on top of the samples (the cause of the gaps)
} AudioCapture_Stats;

typedef enum {
    AUDIO_CAPTURE_OK,        // A block was returned
    AUDIO_CAPTURE_TIMEOUT,   // No block arrived within the timeout
    AUDIO_CAPTURE_STOPPED,   // Capture is not running (or was stopped while waiting)
} AudioCapture_Result;

// Each reader receives every block independently. Not shared between threads.
typedef struct AudioCapture_Reader AudioCapture_Reader;

/**
 * @brief Default settings: ADC channels 0 and 1, 128-frame blocks, the
 *        default SPI clock, about 1.5 s of buffering.
 */
AudioCapture_Config AudioCapture_defaultConfig(void);

/**
 * @brief Initializes the ADC, measures the sample rate, and starts sampling.
 *        Takes about a quarter of a second to measure the rate.
 * @param config Settings to use, or NULL for AudioCapture_defaultConfig().
 * @return true on success; false on bad config, no reserved core, ADC failure, or thread failure.
 */
bool AudioCapture_init(const AudioCapture_Config *config);

/**
 * @brief Stops sampling and releases the ADC. Any reader waiting in
 *        AudioCapture_readBlock() returns AUDIO_CAPTURE_STOPPED. Readers must
 *        still be closed with AudioCapture_closeReader().
 */
void AudioCapture_cleanup(void);

// Settings and measured timing of the running capture. The sample rate is
// measured at startup and then kept up to date from every undisturbed block.
double AudioCapture_getSampleRateHz(void);      // Frames per second inside a block (per microphone)
double AudioCapture_getFramePeriodNs(void);
double AudioCapture_getChannelOffsetNs(int mic);   // How long after mic 0 this mic is sampled in each frame
int AudioCapture_getNumChannels(void);
int AudioCapture_getBlockFrames(void);
uint32_t AudioCapture_getSpiSpeedHz(void);

// The CPU core (reserved with the isolcpus= boot argument) that sampling runs
// on, or AUDIO_CAPTURE_NO_CPU before capture has started.
int AudioCapture_getReservedCpu(void);

/**
 * @brief Copies the sampling statistics (updated once per block).
 */
void AudioCapture_getStats(AudioCapture_Stats *stats);

/**
 * @brief Creates a reader that receives blocks completed from now on.
 * @return The reader, or NULL if capture is not running or out of memory.
 */
AudioCapture_Reader *AudioCapture_openReader(void);

void AudioCapture_closeReader(AudioCapture_Reader *reader);

/**
 * @brief Copies this reader's next block.
 * @param reader    From AudioCapture_openReader().
 * @param samples   Room for getBlockFrames() * getNumChannels() samples. Filled
 *                  frame by frame: [f0 ch0, f0 ch1, f1 ch0, f1 ch1, ...].
 * @param info      Receives the block's details.
 * @param timeoutMs How long to wait for a block: -1 waits forever, 0 never waits.
 */
AudioCapture_Result AudioCapture_readBlock(AudioCapture_Reader *reader, uint16_t *samples,
                                           AudioCapture_BlockInfo *info, int timeoutMs);

#endif
