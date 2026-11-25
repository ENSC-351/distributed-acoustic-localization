// fastmem.c
// Zero the BTCM section at boot, before anything uses it (see fw/r5cap_fastmem.h).
// Zephyr's linker script marks BTCM as NOLOAD, so remoteproc doesn't load it from
// the firmware file and it starts with whatever was in the memory.
#include <stdint.h>
#include <string.h>
#include <zephyr/init.h>

extern char __BTCM_start[];
extern char __BTCM_end[];

static int zeroFastData(void)
{
    memset(__BTCM_start, 0, (size_t)(__BTCM_end - __BTCM_start));
    return 0;
}

// EARLY, priority 0: before the kernel, drivers and every thread
SYS_INIT(zeroFastData, EARLY, 0);
