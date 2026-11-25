// r5cap_ctl.c
// Control the R5 capture firmware from Linux through shared memory
// (r5cap_control.h): no R5 shell or RPMsg involved, so it works while sampling.
//
// Usage on the BeagleY-AI (root, for /dev/mem):
//   sudo ./r5cap_ctl start <frames_per_s> [spi_hz] [irqoff]   (re)start sampling;
//                     irqoff = interrupts off on the R5 while sampling
//   sudo ./r5cap_ctl stop
//   sudo ./r5cap_ctl status
//   sudo ./r5cap_ctl diag      late frames: count, worst, and when (time between them)
#include "r5cap_control.h"
#include "r5cap_shared.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PAGE_BYTES 4096
#define WAIT_MS 2000
#define POLL_US 1000

static void usage(const char *program)
{
    fprintf(stderr, "usage: sudo %s start <frames_per_s> [spi_hz] [irqoff] | stop | status | diag\n",
            program);
}

static void printRing(int fd)
{
    void *map = mmap(NULL, R5CAP_HEADER_SIZE, PROT_READ, MAP_SHARED, fd, R5CAP_SHM_PHYS);
    if (map == MAP_FAILED) {
        return;
    }
    const volatile struct r5cap_header *h = map;
    if (h->magic == R5CAP_MAGIC) {
        printf("ring: state=%u writeSeq=%u lateTicks=%u adcErrors=%u period=%u ticks @ %u Hz\n",
               (unsigned)h->state, (unsigned)h->writeSeq, (unsigned)h->lateTicks,
               (unsigned)h->adcErrors, (unsigned)h->framePeriodTicks, (unsigned)h->tickHz);
    } else {
        printf("ring: not started yet\n");
    }
    munmap(map, R5CAP_HEADER_SIZE);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    int fd = open("/dev/mem", O_RDWR | O_SYNC);   // O_SYNC: uncached mapping
    if (fd < 0) {
        perror("open /dev/mem (run with sudo)");
        return 2;
    }
    void *page = mmap(NULL, PAGE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, R5CAP_HEARTBEAT_PHYS);
    if (page == MAP_FAILED) {
        perror("mmap control page");
        return 2;
    }
    volatile struct r5cap_control *control =
        (volatile struct r5cap_control *)((uint8_t *)page + (R5CAP_CONTROL_PHYS - R5CAP_HEARTBEAT_PHYS));
    if (control->magic != R5CAP_CONTROL_MAGIC) {
        fprintf(stderr, "no control block: is the R5 capture firmware (with r5cap_ctl support) running?\n");
        return 1;
    }

    if (strcmp(argv[1], "diag") == 0) {
        volatile struct r5cap_diag *diag =
            (volatile struct r5cap_diag *)((uint8_t *)page + (R5CAP_DIAG_PHYS - R5CAP_HEARTBEAT_PHYS));
        const double usPerTick = 1e6 / 25e6;
        uint32_t count = diag->eventCount;
        printf("lateEvents=%u maxOvershootUs=%.1f irqoff=%s\n", (unsigned)diag->lateEvents,
               diag->maxOvershootTicks * usPerTick,
               (control->ackFlags & R5CAP_CTL_IRQ_LOCK) ? "yes" : "no");
        uint32_t n = count < R5CAP_DIAG_EVENTS ? count : R5CAP_DIAG_EVENTS;
        if (n > 0) {
            printf("last %u late frames (time since previous one, how late):\n", (unsigned)n);
            uint32_t prev = 0;
            for (uint32_t k = count - n; k < count; k++) {
                uint32_t tick = diag->eventTick[k % R5CAP_DIAG_EVENTS];
                uint32_t over = diag->eventOvershoot[k % R5CAP_DIAG_EVENTS];
                if (k == count - n) {
                    printf("  %8s ms  %6.1f us late\n", "-", over * usPerTick);
                } else {
                    printf("  %8.2f ms  %6.1f us late\n", (uint32_t)(tick - prev) * usPerTick / 1000.0,
                           over * usPerTick);
                }
                prev = tick;
            }
        }
        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        printf("control: seq=%u ackSeq=%u running rate=%u frames/s spi=%u Hz irqoff=%s\n",
               (unsigned)control->seq, (unsigned)control->ackSeq, (unsigned)control->ackRateHz,
               (unsigned)control->ackSpiHz, (control->ackFlags & R5CAP_CTL_IRQ_LOCK) ? "yes" : "no");
        printRing(fd);
        return 0;
    }

    uint32_t rate = 0;
    uint32_t spi = 0;
    uint32_t flags = 0;
    if (strcmp(argv[1], "start") == 0 && argc >= 3) {
        rate = (uint32_t)strtoul(argv[2], NULL, 10);
        spi = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 10) : 0;
        if (argc >= 5 && strcmp(argv[4], "irqoff") == 0) {
            flags |= R5CAP_CTL_IRQ_LOCK;
        }
        if (rate == 0) {
            usage(argv[0]);
            return 2;
        }
    } else if (strcmp(argv[1], "stop") != 0) {
        usage(argv[0]);
        return 2;
    }

    control->rateHz = rate;
    control->spiHz = spi;
    control->flags = flags;
    R5CAP_BARRIER();   // command visible before the new seq
    uint32_t seq = control->seq + 1;
    control->seq = seq;

    for (int waited = 0; control->ackSeq != seq; waited++) {
        if (waited * POLL_US / 1000 >= WAIT_MS) {
            fprintf(stderr, "no acknowledgement from the R5 within %d ms\n", WAIT_MS);
            return 1;
        }
        usleep(POLL_US);
    }
    R5CAP_BARRIER();
    printf("ok: %s, rate=%u frames/s, spi=%u Hz, irqoff=%s\n", rate ? "sampling" : "stopped",
           (unsigned)control->ackRateHz, (unsigned)control->ackSpiHz,
           (control->ackFlags & R5CAP_CTL_IRQ_LOCK) ? "yes" : "no");
    return 0;
}
