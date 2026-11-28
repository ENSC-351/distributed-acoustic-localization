/**
 * @file spi_diag.h
 * @brief Measures where the time goes in each ADC conversion over SPI.
 *
 * Times SPI messages of different transfer lengths, clock speeds and
 * chip-select settings, and works out the real SPI clock, the fixed cost of
 * each transfer, and the cost of releasing chip select between transfers.
 * Sends only zeros, which the MCP3208 ignores (no start bit), so it is safe
 * with the ADC connected. Don't run it while anything else uses the ADC.
 */
#ifndef SPI_DIAG_H
#define SPI_DIAG_H

/**
 * @brief Runs the measurements and prints a report.
 * @return EXIT_SUCCESS, or EXIT_FAILURE if SPI could not be opened.
 */
int SpiDiag_run(void);

#endif
