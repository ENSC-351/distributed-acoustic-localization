// timer_fix.c
// Workaround for a bug in Zephyr v4.5.0's TI DM timer driver
// (drivers/timer/ti_dmtimer.c, unchanged on main as of 2026-11-25).
//
// The driver sets TCLR one bit at a time with read-modify-write: AR, then CE
// (compare enable), then ST (start). The DM timer "posts" TCLR writes: they
// complete a little later in the timer's own clock domain, and until then a
// read returns the old value. So the ST step reads TCLR before the CE write
// has landed and writes it back without CE. The timer then counts but never
// raises its match interrupt, and every k_sleep() blocks forever.
// Seen on the BeagleY-AI MCU R5F: TCLR=0x3 (AR|ST, no CE), IRQSTATUS_RAW=0.
//
// Fix: right after the driver's init, wait until no TCLR write is pending and
// set CE again. Should be fixed in the driver itself (wait on TWPS before each
// read-modify-write); then this file can go.
#include <zephyr/arch/cpu.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define SYSTIMER_BASE DT_REG_ADDR(DT_CHOSEN(zephyr_system_timer))
#define REG_TCLR 0x38
#define REG_TWPS 0x48
#define TCLR_CE BIT(6)          // compare enable
#define TWPS_W_PEND_TCLR BIT(0) // a TCLR write is still being applied

static void waitTclrWrite(void)
{
    while ((sys_read32(SYSTIMER_BASE + REG_TWPS) & TWPS_W_PEND_TCLR) != 0) {
    }
}

static int fixSystemTimerCompare(void)
{
    waitTclrWrite();
    uint32_t tclr = sys_read32(SYSTIMER_BASE + REG_TCLR);
    if ((tclr & TCLR_CE) == 0) {
        sys_write32(tclr | TCLR_CE, SYSTIMER_BASE + REG_TCLR);
        waitTclrWrite();
    }
    return 0;
}

// The driver initializes at PRE_KERNEL_2 / CONFIG_SYSTEM_CLOCK_INIT_PRIORITY;
// run just after it, before any thread can sleep.
BUILD_ASSERT(CONFIG_SYSTEM_CLOCK_INIT_PRIORITY < 1, "must run after the system timer driver");
SYS_INIT(fixSystemTimerCompare, PRE_KERNEL_2, 1);
