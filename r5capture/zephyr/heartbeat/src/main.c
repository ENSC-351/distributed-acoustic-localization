// Heartbeat for the MCU R5F: counts in shared memory so Linux can see that our
// firmware boots, keeps time, and restarts. Linux side: board/heartbeat_read.c
#include <stdint.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/barrier.h>

#include "../../heartbeat.h"

// remoteproc requires a resource table; this one is empty
struct resource_table {
	uint32_t ver;
	uint32_t num;
	uint32_t reserved[2];
};

__attribute__((section(".resource_table"), used))
const struct resource_table resourceTable = { .ver = 1, .num = 0 };

// The tick timer powers up in posted-write mode, where a register read soon
// after a write returns the old value. Zephyr's driver (4.4.2) then loses the
// compare-enable bit and the tick never fires. Turn posted mode off first.
#define TIMER_TSICR_OFFSET 0x54
#define TIMER_TSICR_POSTED BIT(2)

static int disablePostedMode(void)
{
	mem_addr_t tsicr = DT_REG_ADDR(DT_NODELABEL(systick_timer)) + TIMER_TSICR_OFFSET;
	sys_write32(sys_read32(tsicr) & ~TIMER_TSICR_POSTED, tsicr);
	return 0;
}
SYS_INIT(disablePostedMode, PRE_KERNEL_1, 0);

int main(void)
{
	volatile struct r5_heartbeat *hb = (volatile struct r5_heartbeat *)R5_HEARTBEAT_ADDR;

	// Keep counting boots if our own magic is still in memory
	uint32_t boots = (hb->magic == R5_HEARTBEAT_MAGIC) ? hb->boots + 1 : 1;
	hb->magic = 0;
	hb->count = 0;
	hb->uptimeMs = 0;
	hb->boots = boots;
	barrier_dmem_fence_full();
	hb->magic = R5_HEARTBEAT_MAGIC;

	while (1) {
		k_msleep(R5_HEARTBEAT_PERIOD_MS);
		hb->uptimeMs = k_uptime_get_32();
		hb->count++;
	}
	return 0;
}
