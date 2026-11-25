// r5_clock.h (internal to the audio library)
// Converts R5 timer ticks (MCU_TIMER0, ~25 MHz, extended to 64 bits by the
// firmware) to Linux CLOCK_REALTIME nanoseconds.
//
// Two ways to feed it:
//   - Pairs (R5Clock_addPair): Linux reads the same timer directly, between
//     two CLOCK_REALTIME reads. Precise to about the read time (~1 us). The
//     rate is fitted from pairs up to R5CLOCK_HISTORY seconds apart, so the
//     timer's real frequency and chrony's adjustments are both followed.
//   - Arrivals (R5Clock_addArrival), when Linux can't read the timer: the
//     time a block was seen is an upper bound on when it ended. The smallest
//     (arrival - end) over the last R5CLOCK_HISTORY observations is used, at
//     the nominal rate. Precise to about the polling interval.
// A step in CLOCK_REALTIME (more than R5CLOCK_STEP_NS plus R5CLOCK_MAX_RATE_ERROR
// of the time since the last pair, off the fit) restarts the fit from the new pair.
//
// Not thread-safe: the caller locks.
#ifndef R5_CLOCK_H
#define R5_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define R5CLOCK_HISTORY 64            // pairs kept (one per second: about a minute)
#define R5CLOCK_MIN_FIT_S 2.0         // shortest baseline before the rate is fitted
#define R5CLOCK_MAX_RATE_ERROR 0.01   // a fitted rate further than this from nominal is rejected
#define R5CLOCK_STEP_NS 1000000LL     // a pair this far off the fit means the clock was stepped

struct R5Clock {
    double nominalNsPerTick;
    double nsPerTick;                 // fitted (or nominal)
    int count;                        // pairs or arrivals in the history
    int next;                         // ring index of the next entry
    uint64_t tick[R5CLOCK_HISTORY];
    int64_t ns[R5CLOCK_HISTORY];
    bool arrivals;                    // history holds arrivals, not pairs
    uint32_t steps;                   // clock steps detected (diagnostics)
};

void R5Clock_init(struct R5Clock *clock, uint32_t tickHz);

// True once at least one pair or arrival has been added
bool R5Clock_isValid(const struct R5Clock *clock);

// A simultaneous reading of the timer (64-bit) and CLOCK_REALTIME
void R5Clock_addPair(struct R5Clock *clock, uint64_t tick, int64_t realtimeNs);

// A block whose last frame ended at endTick was seen at realtimeNs
void R5Clock_addArrival(struct R5Clock *clock, uint64_t endTick, int64_t realtimeNs);

int64_t R5Clock_toRealtimeNs(const struct R5Clock *clock, uint64_t tick);

// Ticks to nanoseconds at the fitted rate (durations, not times)
double R5Clock_ticksToNs(const struct R5Clock *clock, double ticks);

// Extends a 32-bit timer value to the 64-bit value closest to nearTick
uint64_t R5Clock_extendTick(uint32_t low, uint64_t nearTick);

#endif
