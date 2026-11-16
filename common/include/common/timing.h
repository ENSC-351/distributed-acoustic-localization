// timing.h
// Timing module: helpers for delaying the calling thread and reading clocks.

#ifndef TIMING_H
#define TIMING_H

#include <stdint.h>

#define MS_PER_SECOND 1000

// Block the calling thread for `delayInMs` milliseconds.
void Timing_sleepForMs(long long delayInMs);

// Block the calling thread for `delayInNs` nanoseconds.
void Timing_sleepForNs(long long delayInNs);

// Milliseconds on CLOCK_MONOTONIC. Use this for durations on one board,
// since it never jumps when chrony steps the clock.
long long Timing_getMonotonicMs(void);

// Nanoseconds on CLOCK_REALTIME. Use this for timestamps that get compared
// between boards.
uint64_t Timing_getRealtimeNs(void);

#endif
