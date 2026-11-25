// fastmem.c
// Zero the .r5fast section (ATCM, see r5fast.ld and fw/r5cap_fastmem.h) at boot,
// before anything uses it. It's NOLOAD, so it starts with whatever was in the memory.
#include <stdint.h>
#include <string.h>
#include <zephyr/init.h>

extern char __r5fast_start[];
extern char __r5fast_end[];

static int zeroFastData(void)
{
    memset(__r5fast_start, 0, (size_t)(__r5fast_end - __r5fast_start));
    return 0;
}

// EARLY, priority 0: before the kernel, drivers and every thread
SYS_INIT(zeroFastData, EARLY, 0);
