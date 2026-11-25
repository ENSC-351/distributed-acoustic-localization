// r5cap_status.c
// Prints the R5 capture ring's header counters from Linux (through /dev/mem),
// without disturbing the R5 (asking its shell would steal CPU from sampling).
// Usage on the BeagleY-AI:  sudo ./r5cap_status
// Output, one line:  state=<n> writeSeq=<n> lateTicks=<n> adcErrors=<n> period=<ticks> tickHz=<hz>
#include "r5cap_heartbeat.h"
#include "r5cap_shared.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem (run with sudo)");
        return 2;
    }
    void *map = mmap(NULL, R5CAP_HEADER_SIZE, PROT_READ, MAP_SHARED, fd, R5CAP_SHM_PHYS);
    if (map == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    const volatile struct r5cap_header *h = map;
    if (h->magic != R5CAP_MAGIC) {
        printf("state=none (no ring header: capture never started)\n");
        return 1;
    }
    printf("state=%u writeSeq=%u lateTicks=%u adcErrors=%u period=%u tickHz=%u\n",
           (unsigned)h->state, (unsigned)h->writeSeq, (unsigned)h->lateTicks,
           (unsigned)h->adcErrors, (unsigned)h->framePeriodTicks, (unsigned)h->tickHz);
    return 0;
}
