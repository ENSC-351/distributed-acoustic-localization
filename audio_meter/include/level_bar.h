/**
 * @file level_bar.h
 * @brief Draws 0.0-1.0 levels as text bars, e.g. "[##########          ]  50%"
 *
 * Independent of where the levels come from: works with AudioLevel_getLevel()
 * or any other normalized value.
 */
#ifndef LEVEL_BAR_H
#define LEVEL_BAR_H

#include <stddef.h>

// Most lines LevelBar_printMulti() can draw (bars plus the footer)
#define LEVEL_BAR_MAX_LINES 16

/**
 * @brief Writes one bar into a string buffer (for an LCD, log, network, ...).
 * @param buffer   Destination; always null-terminated if bufferSize > 0.
 * @param bufferSize Size of buffer; needs at least width + 8 bytes for the full bar.
 * @param level    Value to draw, clamped to 0.0-1.0.
 * @param width    Number of characters inside the brackets.
 */
void LevelBar_format(char *buffer, size_t bufferSize, double level, int width);

/**
 * @brief Redraws a single bar in place on the current terminal line.
 *
 * Starts with '\r' and does not end with a newline, so call it repeatedly
 * to animate. Anything in `suffix` (may be NULL) is printed after the bar.
 */
void LevelBar_print(double level, int width, const char *suffix);

/**
 * @brief Redraws several bars in place, one per line (e.g. one per microphone).
 *
 * The first call prints the lines; each later call moves the cursor back up
 * and overwrites them, so call it repeatedly to animate. Don't print anything
 * else in between. Lines are:
 *     <label> [#####     ]  50%<suffix>
 *
 * @param count    Number of bars (at most LEVEL_BAR_MAX_LINES - 1).
 * @param levels   count values, each clamped to 0.0-1.0.
 * @param labels   count strings shown before each bar; the array or any entry may be NULL.
 * @param suffixes count strings shown after each bar; the array or any entry may be NULL.
 * @param footer   Extra line under the bars (e.g. status), or NULL for none.
 * @param width    Number of characters inside each bar's brackets.
 */
void LevelBar_printMulti(int count, const double *levels, const char *const *labels,
                         const char *const *suffixes, const char *footer, int width);

#endif
