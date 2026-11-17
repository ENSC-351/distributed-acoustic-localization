/**
 * @file adc.h
 * @brief HAL for the MCP3208 8-channel, 12-bit ADC over SPI (/dev/spidev0.0)
 *
 * Thread-safe and reference-counted:
 *   - Several modules (microphone, joystick, ...) may each call ADC_init()
 *     and ADC_cleanup(); the SPI device is opened on the first init and
 *     closed on the last cleanup.
 *   - The read functions may be called from multiple threads at once; SPI
 *     messages are serialized internally (so a long ADC_read_frames() call
 *     delays other readers until it finishes).
 */
#ifndef ADC_H
#define ADC_H

#include <stdbool.h>
#include <stdint.h>

// Returned by ADC_read_channel() when the read fails
#define ADC_READ_ERROR (-1)

// Largest value of a 12-bit conversion
#define ADC_MAX_READING 4095

// A signal centered in the input range reads about this much
#define ADC_MIDPOINT ((ADC_MAX_READING + 1) / 2)

// Number of input channels on the MCP3208 (0 through 7)
#define ADC_NUM_CHANNELS 8

// SPI clock. The MCP3208 is specified for 1 MHz at VDD = 2.7 V and 2 MHz at
// 5 V; at 3.3 V a straight-line estimate between the two is about 1.25 MHz.
// The default is the guaranteed 1 MHz. Faster clocks give a higher sample
// rate, but above the chip's limit the readings lose accuracy.
#define ADC_DEFAULT_SPI_SPEED_HZ 1000000
#define ADC_MAX_SPI_SPEED_HZ 2000000

// Most conversions one ADC_read_frames() call can ever do (a Linux limit on
// transfers per SPI message). The real limit on a given system is usually
// lower: see ADC_getMaxConversionsPerRead().
#define ADC_MAX_CONVERSIONS_PER_READ 511

/**
 * @brief Opens and configures the SPI device (if not already open).
 * @return true on success, false if the SPI device could not be set up.
 */
bool ADC_init(void);

/**
 * @brief Releases one ADC_init(); closes SPI when the last user cleans up.
 */
void ADC_cleanup(void);

/**
 * @brief Reads a single ADC channel (single-ended) at the default SPI clock.
 * @param channel ADC channel number, 0 through 7.
 * @return 12-bit value in 0..ADC_MAX_READING, or ADC_READ_ERROR on failure
 *         (not initialized, bad channel, or SPI transfer error).
 */
int ADC_read_channel(uint8_t channel);

/**
 * @brief Reads several channels once each, back to back, in one SPI message.
 *        Same as ADC_read_frames() with one frame at the default SPI clock.
 * @return true if every channel was read, false otherwise.
 */
bool ADC_read_channels(const uint8_t *channels, int count, uint16_t *values);

/**
 * @brief Most conversions one ADC_read_frames() call can do on this system.
 *
 * spidev copies each transfer through a buffer whose size is the
 * spidev.bufsiz setting (4096 bytes by default), rounding every transfer up
 * to the DMA alignment (128 bytes on arm64). So by default only 32 conversions
 * fit in one message. Raise spidev.bufsiz to 65536 to allow the full
 * ADC_MAX_CONVERSIONS_PER_READ. Measured when SPI is opened; call after ADC_init().
 */
int ADC_getMaxConversionsPerRead(void);

/**
 * @brief Describes how conversions are sent over SPI.
 *
 * ADC_init() sends each conversion as one 24-bit SPI word if the driver
 * supports it, it reads the same values, and it is faster than three 8-bit
 * words (the SPI driver spends time on every word). Call after ADC_init().
 */
const char *ADC_getTransferMode(void);

/**
 * @brief Reads many frames in a single SPI message, for sampling audio.
 *
 * A frame is one conversion of each channel, in the order given. All
 * numFrames * numChannels conversions run back to back, so the SPI hardware
 * (not software timing) sets the spacing between samples: every conversion
 * takes the same time, so samples are evenly spaced. One message has far less
 * overhead than one system call per sample.
 *
 * Does not print on failure, so it is safe to call in a loop.
 *
 * @param channels    ADC channel numbers (0 through 7), numChannels of them.
 * @param numChannels Channels per frame, 1 through ADC_NUM_CHANNELS.
 * @param numFrames   Frames to read; numFrames * numChannels may not exceed
 *                    ADC_getMaxConversionsPerRead() (fails with errno EMSGSIZE).
 * @param spiSpeedHz  SPI clock, up to ADC_MAX_SPI_SPEED_HZ, or 0 for the default.
 * @param values      Receives numFrames * numChannels results, frame by frame:
 *                    [f0 ch0, f0 ch1, ..., f1 ch0, f1 ch1, ...].
 * @return true if every conversion was done, false otherwise.
 */
bool ADC_read_frames(const uint8_t *channels, int numChannels, int numFrames,
                     uint32_t spiSpeedHz, uint16_t *values);

#endif
