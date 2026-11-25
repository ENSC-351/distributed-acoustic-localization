// timing.h
// Timing module: helpers for sleeping and for reading the two clocks we use.

#ifndef TIMING_H
#define TIMING_H

#include <stdint.h>

#define MS_PER_SECOND 1000

void Timing_sleepForMs(long long delayInMs);

void Timing_sleepForNs(long long delayInNs);

// CLOCK_MONOTONIC counts up steadily from boot, nothing can mess with it.
// Use this function for durations and timeouts within a board.
// CLOCK_REALTIME can jump when chrony steps it, so it is not good when we want
// a timer to fire at a predictable time
long long Timing_getMonotonicMs(void);

// CLOCK_REALTIME is the wall clock, which should be synced by chrony
// between boards.
// Chrony is allowed to step this time (modify it)
// Use this for timestamps compared between boards.
uint64_t Timing_getRealtimeNs(void);

#endif
