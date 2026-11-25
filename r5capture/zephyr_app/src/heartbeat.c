// heartbeat.c
// Writes struct r5cap_heartbeat (r5cap_heartbeat.h) into the shared region
// every R5CAP_HEARTBEAT_PERIOD_MS, so Linux can see this firmware is running.
// Also adds an "r5cap" command to the Zephyr shell over RPMsg (/dev/ttyRPMSG*).
//
// Caching: the R5's data cache is OFF in this firmware (CONFIG_CACHE_MANAGEMENT
// is not set, so the SoC boot code never enables it; see prj.conf for why it
// must stay off). Writes therefore go straight to DRAM. publish() still has a
// barrier for ordering, and a cache flush that is a no-op today but keeps this
// correct if the cache is ever enabled with a proper MPU setup.
#include "r5cap_heartbeat.h"
#include <zephyr/cache.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/irq.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/sys_io.h>

#define SHM_NODE DT_NODELABEL(r5cap_shm)
BUILD_ASSERT(DT_REG_ADDR(SHM_NODE) == R5CAP_SHM_PHYS,
             "r5cap_shm in the board overlay must match R5CAP_SHM_PHYS");
BUILD_ASSERT(DT_REG_SIZE(SHM_NODE) == R5CAP_SHM_SIZE,
             "r5cap_shm in the board overlay must match R5CAP_SHM_SIZE");

#define HEARTBEAT_STACK_SIZE 1024
#define HEARTBEAT_PRIORITY 7

static struct r5cap_heartbeat *const heartbeat = (struct r5cap_heartbeat *)R5CAP_SHM_PHYS;

// Make our writes visible to Linux, in order (see the caching note above)
static void publish(void)
{
    barrier_dmem_fence_full();
    sys_cache_data_flush_range((void *)heartbeat, sizeof(*heartbeat));
}

static void heartbeatThread(void *unused1, void *unused2, void *unused3)
{
    (void)unused1;
    (void)unused2;
    (void)unused3;

    // Old contents may be left over from a previous run: clear first, set magic last
    heartbeat->magic = 0;
    heartbeat->version = R5CAP_HEARTBEAT_VERSION;
    heartbeat->counter = 0;
    heartbeat->uptimeMs = 0;
    publish();
    heartbeat->magic = R5CAP_HEARTBEAT_MAGIC;
    publish();

    for (;;) {
        k_msleep(R5CAP_HEARTBEAT_PERIOD_MS);
        heartbeat->counter++;
        heartbeat->uptimeMs = k_uptime_get_32();
        publish();
    }
}

K_THREAD_DEFINE(heartbeat_tid, HEARTBEAT_STACK_SIZE, heartbeatThread, NULL, NULL, NULL,
                HEARTBEAT_PRIORITY, 0, 0);

static int cmdStatus(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc;
    (void)argv;
    shell_print(sh, "r5capture heartbeat at 0x%08x: magic=0x%08x counter=%u uptime=%u ms",
                (unsigned int)R5CAP_SHM_PHYS, (unsigned int)heartbeat->magic,
                (unsigned int)heartbeat->counter, (unsigned int)heartbeat->uptimeMs);
    return 0;
}

SHELL_CMD_REGISTER(r5cap, NULL, "Show r5capture heartbeat status", cmdStatus);

// ---- Debug: system timer (MCU_TIMER0) registers, read directly on the R5 ----
// Offsets from zephyr/drivers/timer/ti_dmtimer.h. Used to find out why sleeps
// never wake (timer counts, but its interrupt doesn't arrive).
#define SYSTIMER_NODE DT_CHOSEN(zephyr_system_timer)
#define SYSTIMER_BASE DT_REG_ADDR(SYSTIMER_NODE)
#define SYSTIMER_IRQ DT_IRQN(SYSTIMER_NODE)
#define REG(offset) sys_read32(SYSTIMER_BASE + (offset))

static int cmdTimer(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint32_t count1 = REG(0x3c);
    k_busy_wait(1000);
    uint32_t count2 = REG(0x3c);
    shell_print(sh, "system timer @0x%08x irq %u (irq_is_enabled=%d)", (unsigned int)SYSTIMER_BASE,
                (unsigned int)SYSTIMER_IRQ, irq_is_enabled(SYSTIMER_IRQ));
    shell_print(sh, "  TIDR=0x%08x TIOCP_CFG=0x%08x TCLR=0x%08x", REG(0x00), REG(0x10), REG(0x38));
    shell_print(sh, "  IRQSTATUS_RAW=0x%08x IRQSTATUS=0x%08x IRQENABLE=0x%08x", REG(0x24), REG(0x28),
                REG(0x2c));
    shell_print(sh, "  TCRR=0x%08x then 0x%08x after 1 ms busy-wait (delta %u)", count1, count2,
                count2 - count1);
    shell_print(sh, "  TMAR=0x%08x TLDR=0x%08x TWPS=0x%08x TSICR=0x%08x", REG(0x4c), REG(0x40),
                REG(0x48), REG(0x54));
    shell_print(sh, "  uptime=%u ms, kernel ticks=%u", (unsigned int)k_uptime_get_32(),
                (unsigned int)sys_clock_tick_get_32());
    return 0;
}

SHELL_CMD_REGISTER(r5timer, NULL, "Dump the system timer registers (debug)", cmdTimer);
