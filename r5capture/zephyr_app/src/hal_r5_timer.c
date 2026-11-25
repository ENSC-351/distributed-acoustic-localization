// hal_r5_timer.c
// R5HAL_nowTicks() / R5HAL_tickHz() for fw/r5_hal.h: the system timer
// (MCU_TIMER0, 25 MHz, 32-bit, free-running) extended to 64 bits.
// Only the capture thread calls these, so no locking.
#include "r5_hal.h"
#include "r5cap_fastmem.h"
#include <zephyr/arch/cpu.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/sys_io.h>

#define TIMER_TCRR (DT_REG_ADDR(DT_CHOSEN(zephyr_system_timer)) + 0x3c)
#define TIMER_HZ 25000000u   // measured on the board (25,050 counts per 1 ms busy-wait)

static R5CAP_FAST_DATA uint32_t lastLow = 0;
static R5CAP_FAST_DATA uint64_t high = 0;

R5CAP_FAST_CODE uint64_t R5HAL_nowTicks(void)
{
    uint32_t low = sys_read32(TIMER_TCRR);
    if (low < lastLow) {
        high += 1ull << 32;   // the 32-bit counter wrapped (every ~172 s)
    }
    lastLow = low;
    return high | low;
}

uint32_t R5HAL_tickHz(void)
{
    return TIMER_HZ;
}
