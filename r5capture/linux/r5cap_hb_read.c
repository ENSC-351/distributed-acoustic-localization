// r5cap_hb_read.c
// Linux side of the Phase 1 heartbeat check: maps the r5capture shared region
// through /dev/mem and checks the R5 firmware's heartbeat counter is moving.
// Run as root on the BeagleY-AI:  sudo ./r5cap_hb_read [seconds]
#include "r5cap_heartbeat.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAP_BYTES 4096   // the heartbeat page (R5CAP_HEARTBEAT_PHYS is page-aligned)

int main(int argc, char *argv[])
{
    int seconds = (argc > 1) ? atoi(argv[1]) : 2;
    if (seconds < 1) {
        seconds = 1;
    }

    // O_SYNC maps the memory uncached on Linux, so every read goes to DRAM
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem (run with sudo)");
        return 2;
    }
    void *map = mmap(NULL, MAP_BYTES, PROT_READ, MAP_SHARED, fd, R5CAP_HEARTBEAT_PHYS);
    if (map == MAP_FAILED) {
        perror("mmap heartbeat page");
        close(fd);
        return 2;
    }
    const volatile struct r5cap_heartbeat *hb = map;

    printf("heartbeat at 0x%08X: magic=0x%08X version=%u\n", R5CAP_HEARTBEAT_PHYS,
           (unsigned int)hb->magic, (unsigned int)hb->version);
    uint32_t counter1 = hb->counter;
    uint32_t uptime1 = hb->uptimeMs;
    printf("  t=0 s: counter=%u uptime=%u ms\n", (unsigned int)counter1, (unsigned int)uptime1);
    sleep((unsigned int)seconds);
    uint32_t counter2 = hb->counter;
    uint32_t uptime2 = hb->uptimeMs;
    printf("  t=%d s: counter=%u uptime=%u ms\n", seconds, (unsigned int)counter2,
           (unsigned int)uptime2);

    uint32_t expected = (uint32_t)seconds * 1000u / R5CAP_HEARTBEAT_PERIOD_MS;
    uint32_t moved = counter2 - counter1;
    int ok = hb->magic == R5CAP_HEARTBEAT_MAGIC && moved > 0;
    printf("counter moved by %u in %d s (expected about %u): %s\n", (unsigned int)moved, seconds,
           (unsigned int)expected,
           ok ? "PASS: the R5 is running our firmware"
              : (hb->magic != R5CAP_HEARTBEAT_MAGIC ? "FAIL: no heartbeat magic"
                                                     : "FAIL: counter not moving"));

    munmap(map, MAP_BYTES);
    close(fd);
    return ok ? 0 : 1;
}
