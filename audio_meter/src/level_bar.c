#include "level_bar.h"
#include <stdio.h>
#include <stdlib.h>

#define BAR_FILL_CHAR '#'
#define BAR_EMPTY_CHAR ' '
#define PERCENT_SCALE 100
#define MAX_BAR_WIDTH 200
#define MAX_LINE_LENGTH (MAX_BAR_WIDTH + 128)

// ANSI terminal codes
#define ANSI_CURSOR_UP "\x1b[%dA"
#define ANSI_CLEAR_LINE "\x1b[2K"

// Lines drawn by the previous LevelBar_printMulti() call
static int linesOnScreen = 0;

void LevelBar_format(char *buffer, size_t bufferSize, double level, int width)
{
    if (buffer == NULL || bufferSize == 0) {
        return;
    }
    if (width < 0) {
        width = 0;
    }
    if (width > MAX_BAR_WIDTH) {
        width = MAX_BAR_WIDTH;
    }
    if (level < 0.0) {
        level = 0.0;
    }
    if (level > 1.0) {
        level = 1.0;
    }

    int filled = (int)(level * width + 0.5);
    char bar[MAX_BAR_WIDTH + 1];
    for (int i = 0; i < width; i++) {
        bar[i] = (i < filled) ? BAR_FILL_CHAR : BAR_EMPTY_CHAR;
    }
    bar[width] = '\0';

    snprintf(buffer, bufferSize, "[%s] %3d%%", bar, (int)(level * PERCENT_SCALE + 0.5));
}

void LevelBar_print(double level, int width, const char *suffix)
{
    char line[MAX_BAR_WIDTH + 16];
    LevelBar_format(line, sizeof(line), level, width);
    printf("\r%s%s", line, suffix ? suffix : "");
    fflush(stdout);
}

static void printLine(const char *text)
{
    printf("\r" ANSI_CLEAR_LINE "%s\n", text);
}

void LevelBar_printMulti(int count, const double *levels, const char *const *labels,
                         const char *const *suffixes, const char *footer, int width)
{
    if (levels == NULL || count < 0) {
        return;
    }
    if (count > LEVEL_BAR_MAX_LINES - 1) {
        count = LEVEL_BAR_MAX_LINES - 1;
    }

    if (linesOnScreen > 0) {
        printf(ANSI_CURSOR_UP, linesOnScreen);
    }

    for (int i = 0; i < count; i++) {
        char bar[MAX_BAR_WIDTH + 16];
        char line[MAX_LINE_LENGTH];
        const char *label = (labels && labels[i]) ? labels[i] : "";
        const char *suffix = (suffixes && suffixes[i]) ? suffixes[i] : "";
        LevelBar_format(bar, sizeof(bar), levels[i], width);
        snprintf(line, sizeof(line), "%s%s%s%s", label, label[0] ? " " : "", bar, suffix);
        printLine(line);
    }
    int lines = count;
    if (footer != NULL) {
        printLine(footer);
        lines++;
    }

    linesOnScreen = lines;
    fflush(stdout);
}
