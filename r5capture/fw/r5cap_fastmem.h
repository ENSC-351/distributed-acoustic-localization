// r5cap_fastmem.h
// R5CAP_FAST_DATA marks variables used on every frame. On the R5 build it puts them
// in the TCM at address 0 (fast internal memory: about 1 cycle, vs ~150 ns per access for the
// uncached DRAM where normal variables live), section .r5fast at 0x1000
// (zephyr_app/r5fast.ld). Elsewhere (unit tests on a PC) it does nothing.
//
// R5CAP_FAST_CODE does the same for functions on the sampling path (section
// .r5fast_text, loaded from the firmware file). Code in DRAM competes with Linux for
// memory even with the instruction cache on: with three Linux cores writing memory,
// 1 % of frames at 20,000 frames/s and 28 % at 22,000 were late.
//
// Rules for data: only for variables whose initial value is 0 / false / NULL. The fast
// section isn't loaded from the firmware file; the R5 firmware zeroes it at boot
// (zephyr_app/src/fastmem.c), so non-zero initializers would be lost.
#ifndef R5CAP_FASTMEM_H
#define R5CAP_FASTMEM_H

#if defined(R5CAP_USE_FASTMEM)
#define R5CAP_FAST_DATA __attribute__((section(".r5fast")))
#define R5CAP_FAST_CODE __attribute__((section(".r5fast_text")))
#else
#define R5CAP_FAST_DATA
#define R5CAP_FAST_CODE
#endif

#endif
