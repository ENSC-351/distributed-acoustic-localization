#include "hal/adc.h"
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

#define DEFAULT_SPI_DEVICE "/dev/spidev0.0"
#define SPI_DEVICE_ENV "ADC_SPI_DEVICE"

// MCP3208 command format (24 clocks per conversion):
//   byte 0: 0 0 0 0 0 START SGL D2
//   byte 1: D1 D0 x x x x x x
//   reply:  result bits B11..B8 in the low nibble of byte 1, B7..B0 in byte 2
#define ADC_START_BIT 0x04
#define ADC_SINGLE_ENDED_BIT 0x02
#define ADC_CHANNEL_HIGH_BIT 0x04   // D2, sent in byte 0
#define ADC_CHANNEL_HIGH_SHIFT 2
#define ADC_CHANNEL_LOW_BITS 0x03   // D1 D0, sent at the top of byte 1
#define ADC_CHANNEL_LOW_SHIFT 6
#define ADC_RESULT_HIGH_MASK 0x0F
#define ADC_RESULT_MASK 0x0FFF
#define BITS_PER_BYTE 8
#define BYTES_PER_CONVERSION 3

// A conversion can be sent three ways. The bits on the wire are what costs
// time (the clock is the limit), so fewer bits are faster:
//   - three 8-bit words: 24 clocks, works with any SPI controller
//   - one 24-bit word: the same 24 clocks as one word
//   - one 19-bit word: the fewest clocks an MCP3208 conversion needs. The
//     start bit goes on the first clock, then SGL and D2..D0, one clock to
//     finish sampling, a null bit, and the 12 result bits.
// spidev stores words over 16 bits as native-endian 32-bit values and sends
// the low bits most significant first. ADC_init() checks each word mode reads
// the same values as 8-bit words, and uses the fastest.
typedef enum {
    MODE_BYTES,
    MODE_WORD24,
    MODE_WORD19,
    NUM_MODES
} TransferMode;

static const uint8_t modeBitsPerWord[NUM_MODES] = { 8, 24, 19 };
static const char *const modeNames[NUM_MODES] = {
    "three 8-bit words (24 clocks) per conversion",
    "one 24-bit word (24 clocks) per conversion",
    "one 19-bit word (19 clocks, the minimum) per conversion",
};
#define WORD_LENGTH sizeof(uint32_t)
#define FIRST_BYTE_SHIFT 16
#define WORD19_START_BIT (1u << 18)
#define WORD19_SINGLE_ENDED_BIT (1u << 17)
#define WORD19_CHANNEL_SHIFT 14

// Choosing the transfer mode: compare this many readings of channel 0 with 8-bit
// readings taken alternately, and accept a word mode if the averages differ by at
// most this many counts, or this many standard errors if the input is noisy (a live
// mic moves by more than the fixed limit on its own)
#define MODE_TEST_READS 64
#define MODE_TEST_TOLERANCE 100
#define MODE_TEST_MAX_ERRORS 4
#define MODE_TEST_MESSAGE_LENGTH 8
#define MODE_TEST_ROUNDS 10
#define MODE_TEST_CHANNEL 0

#define NS_PER_S 1000000000LL

static pthread_mutex_t adcMutex = PTHREAD_MUTEX_INITIALIZER;
static int spiFd = -1;
static int initCount = 0;
static TransferMode transferMode = MODE_BYTES;

// Most conversions one SPI message can hold on this system. spidev copies
// each transfer through a buffer (the spidev.bufsiz setting, 4096 bytes by
// default) and rounds every transfer up to the DMA alignment (128 bytes on
// arm64), so by default only 32 conversions fit. Found when SPI is opened.
static int maxConversions = ADC_MAX_CONVERSIONS_PER_READ;

// Buffers for one SPI message. Only used while holding adcMutex.
static uint8_t txBytes[ADC_MAX_CONVERSIONS_PER_READ][BYTES_PER_CONVERSION];
static uint8_t rxBytes[ADC_MAX_CONVERSIONS_PER_READ][BYTES_PER_CONVERSION];
static uint32_t txWords[ADC_MAX_CONVERSIONS_PER_READ];
static uint32_t rxWords[ADC_MAX_CONVERSIONS_PER_READ];
static struct spi_ioc_transfer transfers[ADC_MAX_CONVERSIONS_PER_READ];

static int64_t nowNs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * NS_PER_S + t.tv_nsec;
}

// Opens and configures the SPI device. Caller must hold adcMutex.
static bool openSpi(void)
{
    const char *device = getenv(SPI_DEVICE_ENV);
    if (device == NULL || device[0] == '\0') {
        device = DEFAULT_SPI_DEVICE;
    }
    spiFd = open(device, O_RDWR);
    if (spiFd < 0) {
        fprintf(stderr, "ADC: unable to open %s: %s\n", device, strerror(errno));
        return false;
    }

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = BITS_PER_BYTE;
    // The device maximum; each transfer then asks for the speed it wants
    uint32_t speed = ADC_MAX_SPI_SPEED_HZ;

    if (ioctl(spiFd, SPI_IOC_WR_MODE, &mode) == -1 ||
        ioctl(spiFd, SPI_IOC_WR_BITS_PER_WORD, &bits) == -1 ||
        ioctl(spiFd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) == -1) {
        perror("ADC: unable to configure SPI");
        close(spiFd);
        spiFd = -1;
        return false;
    }
    return true;
}

// The 3 command bytes that start a single-ended conversion on channel
static void fillCommand(uint8_t channel, uint8_t tx[BYTES_PER_CONVERSION])
{
    tx[0] = (uint8_t)(ADC_START_BIT | ADC_SINGLE_ENDED_BIT |
                      ((channel & ADC_CHANNEL_HIGH_BIT) >> ADC_CHANNEL_HIGH_SHIFT));
    tx[1] = (uint8_t)((channel & ADC_CHANNEL_LOW_BITS) << ADC_CHANNEL_LOW_SHIFT);
    tx[2] = 0x00;
}

// Sends one message of count conversions, cycling through the channels.
// Results stay in the receive buffers (see resultLocked()). Caller must hold
// adcMutex. Returns the ioctl() result: negative (with errno set) on failure.
static int sendMessageLocked(const uint8_t *channels, int numChannels, int count, uint32_t spiSpeedHz)
{
    memset(transfers, 0, (size_t)count * sizeof(transfers[0]));
    for (int i = 0; i < count; i++) {
        uint8_t channel = channels[i % numChannels];
        if (transferMode == MODE_BYTES) {
            fillCommand(channel, txBytes[i]);
            transfers[i].tx_buf = (unsigned long)txBytes[i];
            transfers[i].rx_buf = (unsigned long)rxBytes[i];
            transfers[i].len = BYTES_PER_CONVERSION;
        } else {
            if (transferMode == MODE_WORD24) {
                uint8_t command[BYTES_PER_CONVERSION];
                fillCommand(channel, command);
                txWords[i] = ((uint32_t)command[0] << FIRST_BYTE_SHIFT) |
                             ((uint32_t)command[1] << BITS_PER_BYTE) | command[2];
            } else {
                txWords[i] = WORD19_START_BIT | WORD19_SINGLE_ENDED_BIT |
                             ((uint32_t)channel << WORD19_CHANNEL_SHIFT);
            }
            transfers[i].tx_buf = (unsigned long)&txWords[i];
            transfers[i].rx_buf = (unsigned long)&rxWords[i];
            transfers[i].len = WORD_LENGTH;
        }
        transfers[i].bits_per_word = modeBitsPerWord[transferMode];
        transfers[i].speed_hz = spiSpeedHz;
        // The MCP3208 starts each conversion on a falling chip select, so
        // release it between conversions (the last one is released anyway)
        transfers[i].cs_change = (i < count - 1) ? 1 : 0;
    }
    return ioctl(spiFd, SPI_IOC_MESSAGE(count), transfers);
}

// Result of conversion i of the last message. Caller must hold adcMutex.
static uint16_t resultLocked(int i)
{
    if (transferMode != MODE_BYTES) {
        return (uint16_t)(rxWords[i] & ADC_RESULT_MASK);
    }
    return (uint16_t)(((rxBytes[i][1] & ADC_RESULT_HIGH_MASK) << BITS_PER_BYTE) | rxBytes[i][2]);
}

// Fastest time for a short message in the current mode, or -1 on failure.
// Caller must hold adcMutex.
static int64_t timeShortMessageLocked(void)
{
    const uint8_t channel = MODE_TEST_CHANNEL;
    int64_t fastest = INT64_MAX;
    for (int i = 0; i < MODE_TEST_ROUNDS; i++) {
        int64_t start = nowNs();
        if (sendMessageLocked(&channel, 1, MODE_TEST_MESSAGE_LENGTH, ADC_DEFAULT_SPI_SPEED_HZ) < 1) {
            return -1;
        }
        int64_t elapsed = nowNs() - start;
        if (elapsed < fastest) {
            fastest = elapsed;
        }
    }
    return fastest;
}

// One reading of the test channel in the given mode, or -1 if the driver rejects
// the mode. Caller must hold adcMutex.
static long testReadingLocked(TransferMode mode)
{
    const uint8_t channel = MODE_TEST_CHANNEL;
    transferMode = mode;
    if (sendMessageLocked(&channel, 1, 1, ADC_DEFAULT_SPI_SPEED_HZ) < 1) {
        return -1;
    }
    return resultLocked(0);
}

typedef enum {
    MODE_MATCHES,
    MODE_DIFFERS,
    MODE_UNSUPPORTED,
} ModeCheck;

// Compares a word mode with 8-bit words, reading them alternately so that slow
// changes of the input affect both equally. Caller must hold adcMutex.
static ModeCheck checkModeLocked(TransferMode mode)
{
    double sumDiff = 0.0;
    double sumSquares = 0.0;
    for (int i = 0; i < MODE_TEST_READS; i++) {
        long reference = testReadingLocked(MODE_BYTES);
        long value = testReadingLocked(mode);
        if (reference < 0 || value < 0) {
            return MODE_UNSUPPORTED;
        }
        double diff = (double)(value - reference);
        sumDiff += diff;
        sumSquares += diff * diff;
    }
    double mean = sumDiff / MODE_TEST_READS;
    double meanVariance = (sumSquares / MODE_TEST_READS - mean * mean) / MODE_TEST_READS;
    bool matches = mean * mean <= (double)MODE_TEST_TOLERANCE * MODE_TEST_TOLERANCE ||
                   mean * mean <= (double)MODE_TEST_MAX_ERRORS * MODE_TEST_MAX_ERRORS * meanVariance;
    if (!matches) {
        fprintf(stderr, "ADC: %s reads differently from 8-bit words (by %.0f on average), "
                        "so not using it\n", modeNames[mode], mean);
    }
    return matches ? MODE_MATCHES : MODE_DIFFERS;
}

// Picks the fastest transfer mode that the SPI driver accepts and that reads
// the same values as 8-bit words. Caller must hold adcMutex.
static bool chooseTransferMode(void)
{
    transferMode = MODE_BYTES;
    int64_t bestTime = timeShortMessageLocked();
    if (bestTime < 0) {
        perror("ADC: SPI transfer failed");
        return false;
    }
    TransferMode best = MODE_BYTES;

    for (int mode = MODE_BYTES + 1; mode < NUM_MODES; mode++) {
        if (checkModeLocked((TransferMode)mode) != MODE_MATCHES) {
            continue;
        }
        transferMode = (TransferMode)mode;
        int64_t time = timeShortMessageLocked();
        if (time > 0 && time < bestTime) {
            bestTime = time;
            best = (TransferMode)mode;
        }
    }
    transferMode = best;
    return true;
}

// Finds the largest message spidev accepts. Caller must hold adcMutex.
static bool findMaxConversions(void)
{
    const uint8_t channel = MODE_TEST_CHANNEL;
    maxConversions = ADC_MAX_CONVERSIONS_PER_READ;
    if (sendMessageLocked(&channel, 1, maxConversions, ADC_DEFAULT_SPI_SPEED_HZ) >= 0) {
        return true;
    }
    if (errno != EMSGSIZE) {
        perror("ADC: SPI transfer failed");
        return false;
    }

    // Binary search: low always works, high never does
    int low = 0;
    int high = ADC_MAX_CONVERSIONS_PER_READ;
    while (high - low > 1) {
        int middle = (low + high) / 2;
        if (sendMessageLocked(&channel, 1, middle, ADC_DEFAULT_SPI_SPEED_HZ) >= 0) {
            low = middle;
        } else if (errno == EMSGSIZE) {
            high = middle;
        } else {
            perror("ADC: SPI transfer failed");
            return false;
        }
    }
    if (low == 0) {
        fprintf(stderr, "ADC: spidev rejects even a single conversion\n");
        return false;
    }
    maxConversions = low;
    return true;
}

bool ADC_init(void)
{
    pthread_mutex_lock(&adcMutex);
    bool ok = (initCount > 0) || (openSpi() && chooseTransferMode() && findMaxConversions());
    if (!ok && spiFd >= 0) {
        close(spiFd);
        spiFd = -1;
    }
    if (ok) {
        initCount++;
    }
    pthread_mutex_unlock(&adcMutex);
    return ok;
}

void ADC_cleanup(void)
{
    pthread_mutex_lock(&adcMutex);
    if (initCount > 0) {
        initCount--;
        if (initCount == 0) {
            close(spiFd);
            spiFd = -1;
        }
    }
    pthread_mutex_unlock(&adcMutex);
}

int ADC_getMaxConversionsPerRead(void)
{
    pthread_mutex_lock(&adcMutex);
    int value = maxConversions;
    pthread_mutex_unlock(&adcMutex);
    return value;
}

const char *ADC_getTransferMode(void)
{
    pthread_mutex_lock(&adcMutex);
    TransferMode mode = transferMode;
    pthread_mutex_unlock(&adcMutex);
    return modeNames[mode];
}

bool ADC_read_frames(const uint8_t *channels, int numChannels, int numFrames,
                     uint32_t spiSpeedHz, uint16_t *values)
{
    if (channels == NULL || values == NULL || numChannels < 1 || numChannels > ADC_NUM_CHANNELS ||
        numFrames < 1 || numFrames > ADC_MAX_CONVERSIONS_PER_READ / numChannels ||
        spiSpeedHz > ADC_MAX_SPI_SPEED_HZ) {
        errno = EINVAL;
        return false;
    }
    for (int c = 0; c < numChannels; c++) {
        if (channels[c] >= ADC_NUM_CHANNELS) {
            errno = EINVAL;
            return false;
        }
    }
    if (spiSpeedHz == 0) {
        spiSpeedHz = ADC_DEFAULT_SPI_SPEED_HZ;
    }
    const int count = numFrames * numChannels;

    pthread_mutex_lock(&adcMutex);
    if (initCount == 0) {
        pthread_mutex_unlock(&adcMutex);
        errno = ENODEV;
        return false;
    }
    if (count > maxConversions) {
        pthread_mutex_unlock(&adcMutex);
        errno = EMSGSIZE;
        return false;
    }

    bool ok = sendMessageLocked(channels, numChannels, count, spiSpeedHz) >= 1;
    if (ok) {
        for (int i = 0; i < count; i++) {
            values[i] = resultLocked(i);
        }
    }
    pthread_mutex_unlock(&adcMutex);
    return ok;
}

bool ADC_read_channels(const uint8_t *channels, int count, uint16_t *values)
{
    return ADC_read_frames(channels, count, 1, 0, values);
}

int ADC_read_channel(uint8_t channel)
{
    if (channel >= ADC_NUM_CHANNELS) {
        fprintf(stderr, "ADC: invalid channel %u\n", channel);
        return ADC_READ_ERROR;
    }

    pthread_mutex_lock(&adcMutex);
    bool isInitialized = initCount > 0;
    pthread_mutex_unlock(&adcMutex);
    if (!isInitialized) {
        fprintf(stderr, "ADC: ADC_init() must be called before ADC_read_channel()\n");
        return ADC_READ_ERROR;
    }

    uint16_t value;
    if (!ADC_read_channels(&channel, 1, &value)) {
        perror("ADC: SPI transfer failed");
        return ADC_READ_ERROR;
    }
    return value;
}
