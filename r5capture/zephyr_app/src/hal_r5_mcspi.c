// hal_r5_mcspi.c
// R5HAL_spiTransfer() for fw/r5_hal.h using the MCU_SPI0 hardware controller
// (build option R5CAP_SPI=mcspi, the default). The controller shifts the bits,
// so a conversion costs ~5 register accesses instead of ~150 when bit-banging.
//
// Board requirements (board/setup_mcspi_overlay.sh, board/mcspi_power_on.sh):
//   - Linux boots with Joey's MCU_SPI0 overlay: the ADC pads are muxed to MCU_SPI0
//     (D0 = MOSI out, D1 = MISO in) instead of GPIO.
//   - Linux keeps MCU_SPI0 powered and clocked (power/control = "on"). Touching an
//     unpowered module can fault the R5, so the board script checks this first.
//   - Linux must not use the SPI device while the R5 samples.
//
// MCP3208 word: the 3-byte command from fw/mcp3208.c starts with 5 zero bits,
// which the ADC ignores (nothing happens before the start bit). So the 24-bit
// transfer is sent as one 19-bit word, the minimum for a conversion: same
// result bits in the same positions, 21 % fewer clocks.
//
// Register map and bit fields: Zephyr's drivers/spi/spi_omap_mcspi.c.
#include "hal_r5_spi.h"
#include "r5_hal.h"
#include "r5cap_fastmem.h"
#include <zephyr/arch/cpu.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

// MCU_SPI0 base (AM62x/J722S MCU domain). The board script checks Linux's device
// path (spi@4b00000) matches before this firmware is loaded.
#ifndef MCSPI_BASE
#define MCSPI_BASE 0x04B00000u
#endif
// Functional (reference) clock of the controller (48 MHz). R5SPI_measure() reports the
// real SPI clock, so a wrong value here shows up immediately.
#ifndef MCSPI_REF_HZ
#define MCSPI_REF_HZ 48000000u   // measured on the board: shift times at two dividers -> ~48.7 MHz
#endif
#ifndef MCSPI_DEFAULT_HZ
#define MCSPI_DEFAULT_HZ 1000000u   // MCP3208: 1 MHz guaranteed at 2.7 V
#endif

#define REG_SYSCONFIG (MCSPI_BASE + 0x110)
#define REG_SYSSTATUS (MCSPI_BASE + 0x114)
#define REG_MODULCTRL (MCSPI_BASE + 0x128)
#define REG_CH0CONF (MCSPI_BASE + 0x12c)
#define REG_CH0STAT (MCSPI_BASE + 0x130)
#define REG_CH0CTRL (MCSPI_BASE + 0x134)
#define REG_TX0 (MCSPI_BASE + 0x138)
#define REG_RX0 (MCSPI_BASE + 0x13c)

#define SYSCONFIG_SOFTRESET BIT(1)
#define SYSSTATUS_RESETDONE BIT(0)
#define MODULCTRL_SINGLE BIT(0)      // single channel, controller mode, CS by FORCE
#define CHCONF_PHA BIT(0)
#define CHCONF_POL BIT(1)
#define CHCONF_CLKD_SHIFT 2          // 4 bits
#define CHCONF_EPOL BIT(6)           // CS active low
#define CHCONF_WL_SHIFT 7            // word length - 1, 5 bits
#define CHCONF_DPE0 BIT(16)          // 1 = don't transmit on D0
#define CHCONF_DPE1 BIT(17)          // 1 = don't transmit on D1
#define CHCONF_IS BIT(18)            // 1 = receive on D1
#define CHCONF_FORCE BIT(20)         // assert CS
#define CHCONF_CLKG BIT(29)          // divider granularity 1 (uses EXTCLK)
#define CHCTRL_EN BIT(0)
#define CHCTRL_EXTCLK_SHIFT 8        // 8 bits
#define CHSTAT_RXS BIT(0)

#define WORD_BITS 19u
#define WORD_MASK ((1u << WORD_BITS) - 1u)
#define LEADING_ZERO_MASK 0xF80000u  // the 5 bits dropped from the 24-bit command
#define MAX_RATIO 4096u
#define RESET_WAIT_LOOPS 100000
#define TRANSFER_TIMEOUT_TICKS 25000u    // 1 ms: never hang the R5 on a stuck transfer
#define TRANSFER_FAILED 0xFFFFFFFFu      // more than 19 bits, so never a real reply

// MCP3208: CS must stay high at least 500 ns between conversions (tCSH)
#define CS_HIGH_TICKS 14u   // 560 ns at 25 MHz

R5CAP_FAST_DATA volatile bool r5spiProfileOn = false;
R5CAP_FAST_DATA uint32_t r5spiProfileTicks = 0;
R5CAP_FAST_DATA uint32_t r5spiProfileShiftTicks = 0;

static R5CAP_FAST_DATA bool isReady = false;
static R5CAP_FAST_DATA uint32_t chconf = 0;   // CH0CONF without FORCE (kept here to avoid read-modify-write)
static R5CAP_FAST_DATA uint32_t clockHz = 0;
static R5CAP_FAST_DATA uint64_t csHighTick = 0;   // when CS last went high

const char *R5SPI_name(void)
{
    return "mcspi";
}

// Program the divider: SPI clock = MCSPI_REF_HZ / ratio (granularity 1)
static void applyClock(uint32_t hz)
{
    uint32_t ratio = DIV_ROUND_UP(MCSPI_REF_HZ, hz);
    ratio = CLAMP(ratio, 1u, MAX_RATIO);
    chconf = (chconf & ~(0xFu << CHCONF_CLKD_SHIFT)) | (((ratio - 1) & 0xF) << CHCONF_CLKD_SHIFT) |
             CHCONF_CLKG;
    sys_write32(0, REG_CH0CTRL);   // disable while reconfiguring
    sys_write32(chconf, REG_CH0CONF);
    sys_write32((((ratio - 1) >> 4) << CHCTRL_EXTCLK_SHIFT) | CHCTRL_EN, REG_CH0CTRL);
    clockHz = MCSPI_REF_HZ / ratio;
}

static bool init(void)
{
    sys_write32(SYSCONFIG_SOFTRESET, REG_SYSCONFIG);
    for (int i = 0; (sys_read32(REG_SYSSTATUS) & SYSSTATUS_RESETDONE) == 0; i++) {
        if (i >= RESET_WAIT_LOOPS) {
            return false;
        }
    }
    sys_write32(MODULCTRL_SINGLE, REG_MODULCTRL);
    // Mode 0, CS active low, 19-bit words, transmit on D0 only, receive on D1
    chconf = CHCONF_EPOL | ((WORD_BITS - 1) << CHCONF_WL_SHIFT) | CHCONF_DPE1 | CHCONF_IS;
    applyClock(clockHz != 0 ? clockHz : MCSPI_DEFAULT_HZ);
    isReady = true;
    return true;
}

uint32_t R5SPI_setClockHz(uint32_t hz)
{
    if (hz == 0 || (!isReady && !init())) {
        return 0;
    }
    applyClock(hz);
    return clockHz;
}

// One 19-bit word, CS asserted around it. Returns the received word, or
// TRANSFER_FAILED if the controller never finished.
R5CAP_FAST_CODE static inline uint32_t transferWord(uint32_t word, uint32_t *shiftTicks, uint64_t *startTick)
{
    while (R5HAL_nowTicks() - csHighTick < CS_HIGH_TICKS) {
    }
    sys_write32(chconf | CHCONF_FORCE, REG_CH0CONF);   // CS low
    // Let the CS change reach the controller before loading the word. Without a gap
    // here the word is never shifted out (seen on the board once this code ran from
    // TCM). The timer read also covers tSUCS (100 ns).
    (void)sys_read32(REG_CH0CONF);
    const uint64_t start = R5HAL_nowTicks();
    if (startTick != NULL) {
        *startTick = start;
    }
    sys_write32(word, REG_TX0);
    while ((sys_read32(REG_CH0STAT) & CHSTAT_RXS) == 0) {
        if (R5HAL_nowTicks() - start > TRANSFER_TIMEOUT_TICKS) {
            sys_write32(chconf, REG_CH0CONF);
            isReady = false;   // reset the controller before the next transfer
            return TRANSFER_FAILED;
        }
    }
    if (shiftTicks != NULL) {
        *shiftTicks = (uint32_t)(R5HAL_nowTicks() - *startTick);
    }
    uint32_t rx = sys_read32(REG_RX0) & WORD_MASK;
    sys_write32(chconf, REG_CH0CONF);                  // CS high: ends the conversion
    (void)sys_read32(REG_CH0CONF);                     // wait until the write has arrived
    csHighTick = R5HAL_nowTicks();
    return rx;
}

R5CAP_FAST_CODE bool R5HAL_spiTransfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    if (tx == NULL || rx == NULL || len != 3 || (!isReady && !init())) {
        return false;
    }
    uint32_t command = ((uint32_t)tx[0] << 16) | ((uint32_t)tx[1] << 8) | tx[2];
    if ((command & LEADING_ZERO_MASK) != 0) {
        return false;   // only MCP3208-style commands (5 leading zeros) can be shortened
    }
    uint32_t reply;
    if (r5spiProfileOn) {
        uint64_t begin = R5HAL_nowTicks();
        uint64_t shiftStart = 0;
        uint32_t shift = 0;
        reply = transferWord(command & WORD_MASK, &shift, &shiftStart);
        r5spiProfileTicks += (uint32_t)(R5HAL_nowTicks() - begin);
        r5spiProfileShiftTicks += shift;
    } else {
        reply = transferWord(command & WORD_MASK, NULL, NULL);
    }
    if (reply == TRANSFER_FAILED) {
        return false;
    }
    rx[0] = 0;
    rx[1] = (uint8_t)(reply >> 8);
    rx[2] = (uint8_t)reply;
    return true;
}

bool R5SPI_measure(uint8_t channel, uint16_t *value, uint32_t *shiftTicks, uint32_t *totalTicks)
{
    if (channel > 7 || (!isReady && !init())) {
        return false;
    }
    // START | SGL | channel: the same bits fw/mcp3208.c sends, minus the 5 leading zeros
    uint32_t word = (1u << 18) | (1u << 17) | ((uint32_t)channel << 14);
    uint64_t begin = R5HAL_nowTicks();
    uint64_t shiftStart = 0;
    uint32_t reply = transferWord(word, shiftTicks, &shiftStart);
    if (reply == TRANSFER_FAILED) {
        return false;
    }
    *totalTicks = (uint32_t)(R5HAL_nowTicks() - begin);
    *value = (uint16_t)(reply & 0xFFF);
    return true;
}
