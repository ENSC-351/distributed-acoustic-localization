// fatal.c
// On any fatal error (data abort, assert, ...) record why in shared memory
// (struct r5cap_crash, r5cap_control.h) so Linux can read it with
// "r5cap_ctl crash", then halt. Without this, a crash just looks like "the R5
// stopped answering". Look up the PC with:
//   arm-zephyr-eabi-addr2line -e r5capture.elf -f <pc>
#include "r5cap_control.h"
#include <zephyr/arch/cpu.h>
#include <zephyr/fatal.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/sys/barrier.h>

static volatile struct r5cap_crash *const crash = (struct r5cap_crash *)R5CAP_CRASH_PHYS;

static inline uint32_t readCp15(int which)
{
    uint32_t v = 0;
    switch (which) {
    case 0: __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(v)); break;  // DFSR
    case 1: __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(v)); break;  // DFAR
    case 2: __asm__ volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(v)); break;  // IFSR
    default: __asm__ volatile("mrc p15, 0, %0, c6, c0, 2" : "=r"(v)); break; // IFAR
    }
    return v;
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    (void)irq_lock();
    crash->reason = reason;
    crash->pc = (esf != NULL) ? esf->basic.pc : 0;
    crash->lr = (esf != NULL) ? esf->basic.lr : 0;
    crash->r0 = (esf != NULL) ? esf->basic.r0 : 0;
    crash->xpsr = (esf != NULL) ? esf->basic.xpsr : 0;
    crash->dfsr = readCp15(0);
    crash->dfar = readCp15(1);
    crash->ifsr = readCp15(2);
    crash->ifar = readCp15(3);
    barrier_dmem_fence_full();
    crash->magic = R5CAP_CRASH_MAGIC;
    for (;;) {
    }
}

static int clearCrashRecord(void)
{
    crash->magic = 0;
    return 0;
}

SYS_INIT(clearCrashRecord, EARLY, 1);
