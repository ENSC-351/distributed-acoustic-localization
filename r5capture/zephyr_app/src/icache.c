// icache.c
// Turn on the R5's INSTRUCTION cache only, so code running from DRAM is fast.
//
// Why: this firmware runs from DRAM (0xA1200000) with both caches off, and the
// SoC boot springboard maps all DRAM (MPU region 0) as inner NON-cacheable. So
// every instruction is fetched from DDR. Measured on the board (McSPI build):
// a frame cost ~64 us of which only ~32 us was SPI shifting, capping the R5 at
// ~15,700 frames/s, and slow code made every interruption long enough to make
// frames late.
//
// What: (1) mark MPU region 0 (all of DRAM) inner+outer write-back, so its CODE
// can be cached; (2) invalidate and enable the I-cache (SCTLR.I).
// The DATA cache stays OFF (SCTLR.C = 0), so every data access, including the
// shared ring that Linux reads, still goes straight to DRAM, and device
// registers are unaffected (they're outside region 0). Enabling the data cache
// is what stalled the firmware before (see prj.conf); this doesn't do that.
// Safe for code: remoteproc loads the firmware before the R5 starts, and the
// cache is invalidated before it's enabled.
#include <zephyr/arch/arm/cortex_a_r/cpu.h>
#include <zephyr/arch/arm/cortex_a_r/mpu.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>

#define MPU_REGION_DRAM 0   // set up by soc/ti/k3/common/cortex_r/boot.S
// Normal memory, outer and inner write-back write-allocate: TEX=0b001, C=1, B=1
#define DRAM_CACHE_ATTR ((1UL << MPU_RASR_TEX_Pos) | MPU_RASR_C_Msk | MPU_RASR_B_Msk)

// Peripheral registers must be Device memory. Below 0x80000000 the R5's default map is
// Normal memory, where writes to one register may be merged or reordered: with the
// sampling loop in TCM, "CS high" and the next "CS low" can run together. The SoC code only sets up DRAM, so add the two areas we use.
#define MPU_REGION_MCU_PERIPHERALS 6     // timers, MCSPI, VIM
#define MCU_PERIPHERALS_BASE 0x04000000u
#define MCU_PERIPHERALS_SIZE_FIELD 25u   // 2^(25+1) = 64 MB
#define MPU_REGION_MAILBOX 7
#define MAILBOX_BASE 0x29000000u
#define MAILBOX_SIZE_FIELD 19u           // 2^(19+1) = 1 MB
#define DEVICE_ATTR (MPU_RASR_XN_Msk | (3UL << MPU_RASR_AP_Pos) | MPU_RASR_S_Msk | MPU_RASR_B_Msk)

static void setDeviceRegion(uint32_t region, uint32_t base, uint32_t sizeField)
{
    __asm__ volatile("mcr p15, 0, %0, c6, c2, 0" ::"r"(region));                  // RGNR
    __asm__ volatile("isb");
    __asm__ volatile("mcr p15, 0, %0, c6, c1, 0" ::"r"(base));                    // DRBAR
    __asm__ volatile("mcr p15, 0, %0, c6, c1, 4" ::"r"(DEVICE_ATTR));             // DRACR
    __asm__ volatile("mcr p15, 0, %0, c6, c1, 2" ::"r"((sizeField << 1) | 1u));   // DRSR
    __asm__ volatile("dsb; isb" ::: "memory");
}

static inline uint32_t readSctlr(void)
{
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v));
    return v;
}

static int enableInstructionCache(void)
{
    setDeviceRegion(MPU_REGION_MCU_PERIPHERALS, MCU_PERIPHERALS_BASE, MCU_PERIPHERALS_SIZE_FIELD);
    setDeviceRegion(MPU_REGION_MAILBOX, MAILBOX_BASE, MAILBOX_SIZE_FIELD);

    // (1) Region 0 attributes (DRACR): keep access permissions and XN, change the
    //     memory type to cacheable write-back
    uint32_t dracr;
    __asm__ volatile("mcr p15, 0, %0, c6, c2, 0" ::"r"(MPU_REGION_DRAM));   // RGNR
    __asm__ volatile("isb");
    __asm__ volatile("mrc p15, 0, %0, c6, c1, 4" : "=r"(dracr));            // DRACR
    dracr &= ~(MPU_RASR_TEX_Msk | MPU_RASR_C_Msk | MPU_RASR_B_Msk);
    dracr |= DRAM_CACHE_ATTR;
    __asm__ volatile("dsb");
    __asm__ volatile("mcr p15, 0, %0, c6, c1, 4" ::"r"(dracr) : "memory");
    __asm__ volatile("dsb; isb");

    // (2) Invalidate the whole I-cache, then enable it (data cache untouched)
    uint32_t sctlr = readSctlr();
    if ((sctlr & SCTLR_I_BIT) == 0) {
        __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" ::"r"(0) : "memory");    // ICIALLU
        __asm__ volatile("dsb; isb");
        __asm__ volatile("mcr p15, 0, %0, c1, c0, 0" ::"r"(sctlr | SCTLR_I_BIT) : "memory");
        __asm__ volatile("isb");
    }
    return 0;
}

// As early as possible, so the whole boot and everything after runs cached
SYS_INIT(enableInstructionCache, EARLY, 0);

// For the shell: report what's on
uint32_t R5_icacheStatus(void);
uint32_t R5_icacheStatus(void)
{
    return readSctlr();
}
