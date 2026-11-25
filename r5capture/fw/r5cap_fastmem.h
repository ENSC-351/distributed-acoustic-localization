// r5cap_fastmem.h
// R5CAP_FAST_DATA marks variables used on every frame. On the R5 build it puts them
// in the BTCM (fast internal memory: about 1 cycle, vs ~150 ns per access for the
// uncached DRAM where normal variables live). Elsewhere (unit tests on a PC) it does
// nothing.
//
// Rules: only for variables whose initial value is 0 / false / NULL. The BTCM
// section isn't loaded from the firmware file; the R5 firmware zeroes it at boot
// (zephyr_app/src/fastmem.c), so non-zero initializers would be lost.
#ifndef R5CAP_FASTMEM_H
#define R5CAP_FASTMEM_H

#if defined(R5CAP_USE_BTCM)
#define R5CAP_FAST_DATA __attribute__((section("BTCM")))
#else
#define R5CAP_FAST_DATA
#endif

#endif
