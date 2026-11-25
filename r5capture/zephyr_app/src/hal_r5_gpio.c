// hal_r5_gpio.c
// R5HAL_spiTransfer() for fw/r5_hal.h, bit-banged (build option R5CAP_SPI=gpio).
// Measured: ~100 us per 24-bit conversion (each R5 register access ~0.7 us),
// so it tops out around 4,500 frames/s. Kept as a fallback; hal_r5_mcspi.c is
// the fast version.
//
// SPI: bit-banged on MCU_GPIO0, on the pins Linux's course overlay (spidev0,
// spi-gpio) already set up as GPIOs, so nothing on the Linux side changes:
//   CS0 = bit 0 (header 24), SCLK = bit 2 (23), MOSI = bit 3 (19), MISO = bit 4 (21).
// Linux has already set the directions (CS/SCLK/MOSI out, MISO in) and the pad
// muxing. We only use the per-bit SET/CLR registers (no read-modify-write), so
// we never disturb other pins of the bank. Linux must not use /dev/spidev0.x
// while the R5 samples (both would drive the same pins).
//
// Mode 0 (clock idles low, data read on the rising edge), MSB first. The clock
// is timed from the system timer so it stays at or below the MCP3208's 1 MHz
// rating at 2.7 V; HALF_PERIOD_TICKS sets it.
#include "r5_hal.h"
#include <zephyr/arch/cpu.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

// MCU_GPIO0 bank 0 registers (Zephyr's gpio_davinci.c layout), base from the device tree
#define GPIO_BASE DT_REG_ADDR(DT_NODELABEL(mcu_gpio0))
#define GPIO_SET_DATA (GPIO_BASE + 0x08)
#define GPIO_CLR_DATA (GPIO_BASE + 0x0c)
#define GPIO_IN_DATA (GPIO_BASE + 0x10)

#define PIN_CS BIT(0)
#define PIN_SCLK BIT(2)
#define PIN_MOSI BIT(3)
#define PIN_MISO BIT(4)

// System timer: free-running counter
#define TIMER_TCRR (DT_REG_ADDR(DT_CHOSEN(zephyr_system_timer)) + 0x3c)

// 13 ticks = 520 ns half period -> ~0.96 MHz SPI clock (MCP3208: 1 MHz at 2.7 V)
#ifndef HALF_PERIOD_TICKS
#define HALF_PERIOD_TICKS 13u
#endif


static inline uint32_t counterNow(void)
{
    return sys_read32(TIMER_TCRR);
}

static inline void waitHalfPeriod(uint32_t *edge)
{
    // Wait until HALF_PERIOD_TICKS after the previous edge (not after this call),
    // so the time spent in GPIO writes counts toward the half period
    while ((uint32_t)(counterNow() - *edge) < HALF_PERIOD_TICKS) {
    }
    *edge += HALF_PERIOD_TICKS;
}

bool R5HAL_spiTransfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    if (tx == NULL || rx == NULL) {
        return false;
    }
    uint32_t edge = counterNow();
    sys_write32(PIN_SCLK, GPIO_CLR_DATA);   // mode 0: clock idles low
    sys_write32(PIN_CS, GPIO_CLR_DATA);     // select the ADC
    waitHalfPeriod(&edge);

    for (uint32_t i = 0; i < len; i++) {
        uint8_t in = 0;
        for (int bit = 7; bit >= 0; bit--) {   // MSB first
            sys_write32(PIN_MOSI, ((tx[i] >> bit) & 1) ? GPIO_SET_DATA : GPIO_CLR_DATA);
            waitHalfPeriod(&edge);
            sys_write32(PIN_SCLK, GPIO_SET_DATA);           // rising edge: both sides sample
            if ((sys_read32(GPIO_IN_DATA) & PIN_MISO) != 0) {
                in |= (uint8_t)(1u << bit);
            }
            waitHalfPeriod(&edge);
            sys_write32(PIN_SCLK, GPIO_CLR_DATA);           // falling edge: ADC shifts out next bit
        }
        rx[i] = in;
    }

    sys_write32(PIN_CS, GPIO_SET_DATA);     // deselect: ends the conversion
    waitHalfPeriod(&edge);                  // CS high time before the next conversion
    return true;
}

// ---- Extra controls (hal_r5_spi.h) ----
#include "hal_r5_spi.h"
#include "mcp3208.h"

volatile bool r5spiProfileOn = false;   // not implemented for the bit-banged SPI
uint32_t r5spiProfileTicks = 0;
uint32_t r5spiProfileShiftTicks = 0;

const char *R5SPI_name(void)
{
    return "gpio";
}

uint32_t R5SPI_setClockHz(uint32_t hz)
{
    (void)hz;
    return 0;   // fixed by HALF_PERIOD_TICKS and register access time
}

bool R5SPI_measure(uint8_t channel, uint16_t *value, uint32_t *shiftTicks, uint32_t *totalTicks)
{
    uint8_t tx[MCP3208_TRANSFER_BYTES];
    uint8_t rx[MCP3208_TRANSFER_BYTES];
    if (!MCP3208_encodeCommand(channel, tx)) {
        return false;
    }
    uint64_t begin = R5HAL_nowTicks();
    bool ok = R5HAL_spiTransfer(tx, rx, MCP3208_TRANSFER_BYTES);
    *totalTicks = (uint32_t)(R5HAL_nowTicks() - begin);
    *shiftTicks = *totalTicks;
    *value = MCP3208_decodeResult(rx);
    return ok;
}
