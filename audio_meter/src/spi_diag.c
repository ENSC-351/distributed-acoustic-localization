#include "spi_diag.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SPI_DEVICE "/dev/spidev0.0"
#define SPI_DEVICE_ENV "ADC_SPI_DEVICE"
#define SPI_DEVICE_PREFIX "/dev/spidev"
#define DT_MAX_FREQUENCY_FORMAT "/sys/bus/spi/devices/spi%s/of_node/spi-max-frequency"
#define SPIDEV_BUFSIZ "/sys/module/spidev/parameters/bufsiz"

// 32 transfers of up to 4 bytes fit in spidev's default 4096-byte buffer
#define DIAG_TRANSFERS 32
#define DIAG_MAX_LENGTH 4
#define DIAG_SHORT_LENGTH 1
#define DIAG_LONG_LENGTH 4
#define DIAG_ADC_LENGTH 3
#define DIAG_ROUNDS 50
#define DIAG_MAX_SPEED_HZ 2000000
#define BITS_PER_BYTE 8
#define DIAG_WORD_BITS 24
#define DIAG_SHORT_WORD_BITS 19

#define NS_PER_S 1000000000LL
#define NS_PER_US 1000.0
#define HZ_PER_MHZ 1000000.0
#define BYTE_SHIFT 8
#define DT_CELL_BYTES 4

static int64_t nowNs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * NS_PER_S + t.tv_nsec;
}

// Fastest time (ns) per transfer for a message of `count` transfers of
// `length` bytes each, or a negative number if the message failed
static double timeTransfers(int fd, int count, int length, int bitsPerWord, bool csChange, uint32_t speedHz)
{
    static uint8_t tx[DIAG_TRANSFERS][DIAG_MAX_LENGTH];
    static uint8_t rx[DIAG_TRANSFERS][DIAG_MAX_LENGTH];
    struct spi_ioc_transfer transfers[DIAG_TRANSFERS];
    memset(tx, 0, sizeof(tx));
    memset(transfers, 0, sizeof(transfers));

    for (int i = 0; i < count; i++) {
        transfers[i].tx_buf = (unsigned long)tx[i];
        transfers[i].rx_buf = (unsigned long)rx[i];
        transfers[i].len = (uint32_t)length;
        transfers[i].speed_hz = speedHz;
        transfers[i].bits_per_word = (uint8_t)bitsPerWord;
        transfers[i].cs_change = (csChange && i < count - 1) ? 1 : 0;
    }

    int64_t fastest = INT64_MAX;
    for (int round = 0; round < DIAG_ROUNDS; round++) {
        int64_t start = nowNs();
        if (ioctl(fd, SPI_IOC_MESSAGE(count), transfers) < 1) {
            perror("SPI diag: transfer failed");
            return -1.0;
        }
        int64_t elapsed = nowNs() - start;
        if (elapsed < fastest) {
            fastest = elapsed;
        }
    }
    return (double)fastest / count;
}

static double timePerTransfer(int fd, int count, int length, bool csChange, uint32_t speedHz)
{
    return timeTransfers(fd, count, length, BITS_PER_BYTE, csChange, speedHz);
}

static void printFileValue(const char *label, const char *path, bool isDeviceTreeCell)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        printf("  %-28s (not available: %s)\n", label, path);
        return;
    }
    if (isDeviceTreeCell) {
        // Device tree numbers are 4-byte big-endian
        uint8_t bytes[DT_CELL_BYTES];
        if (fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes)) {
            uint32_t value = 0;
            for (int i = 0; i < DT_CELL_BYTES; i++) {
                value = (value << BYTE_SHIFT) | bytes[i];
            }
            printf("  %-28s %u Hz\n", label, value);
        }
    } else {
        char text[64];
        if (fgets(text, sizeof(text), file) != NULL) {
            text[strcspn(text, "\n")] = '\0';
            printf("  %-28s %s\n", label, text);
        }
    }
    fclose(file);
}

int SpiDiag_run(void)
{
    static const uint32_t speedsHz[] = { 250000, 500000, 1000000, 2000000 };
    const int numSpeeds = (int)(sizeof(speedsHz) / sizeof(speedsHz[0]));

    const char *device = getenv(SPI_DEVICE_ENV);
    if (device == NULL || device[0] == '\0') {
        device = DEFAULT_SPI_DEVICE;
    }
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "SPI diag: unable to open %s: %s\n", device, strerror(errno));
        return EXIT_FAILURE;
    }
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = BITS_PER_BYTE;
    uint32_t maxSpeed = DIAG_MAX_SPEED_HZ;
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) == -1 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) == -1 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &maxSpeed) == -1) {
        perror("SPI diag: unable to configure SPI");
        close(fd);
        return EXIT_FAILURE;
    }

    printf("SPI settings (%s):\n", device);
    if (strncmp(device, SPI_DEVICE_PREFIX, strlen(SPI_DEVICE_PREFIX)) == 0) {
        char path[256];
        snprintf(path, sizeof(path), DT_MAX_FREQUENCY_FORMAT, device + strlen(SPI_DEVICE_PREFIX));
        printFileValue("device tree max frequency:", path, true);
    }
    printFileValue("spidev buffer size:", SPIDEV_BUFSIZ, false);
    uint32_t readBack = 0;
    if (ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &readBack) != -1) {
        printf("  %-28s %u Hz\n", "spidev max speed (read back):", readBack);
    }

    printf("\nTime per transfer (us), fastest of %d messages of %d transfers.\n"
           "From the 1-byte vs 4-byte difference: the real SPI clock, and the fixed\n"
           "cost of each transfer that doesn't depend on the clock.\n\n",
           DIAG_ROUNDS, DIAG_TRANSFERS);
    printf("%10s %8s %8s %8s %10s %10s %12s\n", "asked MHz", "1 byte", "3 bytes", "4 bytes",
           "real MHz", "fixed us", "3B no CS us");

    for (int i = 0; i < numSpeeds; i++) {
        double shortNs = timePerTransfer(fd, DIAG_TRANSFERS, DIAG_SHORT_LENGTH, true, speedsHz[i]);
        double adcNs = timePerTransfer(fd, DIAG_TRANSFERS, DIAG_ADC_LENGTH, true, speedsHz[i]);
        double longNs = timePerTransfer(fd, DIAG_TRANSFERS, DIAG_LONG_LENGTH, true, speedsHz[i]);
        double noCsNs = timePerTransfer(fd, DIAG_TRANSFERS, DIAG_ADC_LENGTH, false, speedsHz[i]);
        if (shortNs < 0 || adcNs < 0 || longNs < 0 || noCsNs < 0) {
            close(fd);
            return EXIT_FAILURE;
        }
        double perByteNs = (longNs - shortNs) / (DIAG_LONG_LENGTH - DIAG_SHORT_LENGTH);
        double realMhz = perByteNs > 0 ? BITS_PER_BYTE * NS_PER_US / perByteNs : 0.0;
        double fixedNs = shortNs - perByteNs;
        printf("%10.2f %8.1f %8.1f %8.1f %10.2f %10.1f %12.1f\n",
               speedsHz[i] / HZ_PER_MHZ, shortNs / NS_PER_US, adcNs / NS_PER_US, longNs / NS_PER_US,
               realMhz, fixedNs / NS_PER_US, noCsNs / NS_PER_US);
    }

    double singleNs = timePerTransfer(fd, 1, DIAG_ADC_LENGTH, true, speedsHz[2]);
    printf("\nOne 3-byte transfer alone at 1 MHz (includes the system call): %.1f us\n",
           singleNs / NS_PER_US);

    // One conversion as fewer, bigger SPI words. Words over 16 bits are
    // stored as 4 bytes. Zeros only, so the ADC ignores them.
    printf("\nOne conversion (24 bits) per transfer, sent as different word sizes, at 1 MHz:\n");
    printf("  %-22s %8.1f us\n", "3 x 8-bit words:",
           timeTransfers(fd, DIAG_TRANSFERS, DIAG_ADC_LENGTH, BITS_PER_BYTE, true, speedsHz[2]) / NS_PER_US);
    printf("  %-22s %8.1f us\n", "1 x 24-bit word:",
           timeTransfers(fd, DIAG_TRANSFERS, DIAG_LONG_LENGTH, DIAG_WORD_BITS, true, speedsHz[2]) / NS_PER_US);
    printf("  %-22s %8.1f us   (the fewest clocks an MCP3208 conversion needs)\n", "1 x 19-bit word:",
           timeTransfers(fd, DIAG_TRANSFERS, DIAG_LONG_LENGTH, DIAG_SHORT_WORD_BITS, true, speedsHz[2]) / NS_PER_US);
    printf("\n3 bytes:      the ADC's transfer size; this is the cost of one conversion\n"
           "real MHz:     SPI clock worked out from the timing; should match 'asked MHz'\n"
           "fixed us:     per-transfer cost apart from clocking bits (driver, chip select)\n"
           "3B no CS us:  3-byte transfers without releasing chip select between them;\n"
           "              the difference from '3 bytes' is the chip-select cost\n");
    close(fd);
    return EXIT_SUCCESS;
}
