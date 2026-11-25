// Watches the R5 heartbeat and compares its clock with Linux's. Run as root.
#include "heartbeat.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAP_LENGTH 4096
#define WATCH_SECONDS 5
#define NS_PER_S 1000000000LL
#define NS_PER_MS 1000000.0

static long long nowNs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * NS_PER_S + t.tv_nsec;
}

int main(void)
{
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return EXIT_FAILURE;
    }
    volatile struct r5_heartbeat *hb = mmap(NULL, MAP_LENGTH, PROT_READ, MAP_SHARED, fd, (off_t)R5_HEARTBEAT_ADDR);
    if (hb == MAP_FAILED) {
        perror("mmap");
        return EXIT_FAILURE;
    }
    printf("magic=%08x (%s) boots=%u\n", hb->magic, hb->magic == R5_HEARTBEAT_MAGIC ? "ok" : "BAD", hb->boots);
    uint32_t count0 = hb->count;
    uint32_t uptime0 = hb->uptimeMs;
    long long start = nowNs();
    sleep(WATCH_SECONDS);
    uint32_t counts = hb->count - count0;
    uint32_t r5Ms = hb->uptimeMs - uptime0;
    printf("%u beats, R5 clock advanced %u ms, Linux %.0f ms\n", counts, r5Ms, (double)(nowNs() - start) / NS_PER_MS);
    return (hb->magic == R5_HEARTBEAT_MAGIC && counts > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
