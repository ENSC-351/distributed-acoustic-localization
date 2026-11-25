// r5_clock.c: see r5_clock.h
#include "r5_clock.h"

#include <stddef.h>

static int newestIndex(const struct R5Clock *clock)
{
    return (clock->next + R5CLOCK_HISTORY - 1) % R5CLOCK_HISTORY;
}

static int oldestIndex(const struct R5Clock *clock)
{
    return (clock->count < R5CLOCK_HISTORY) ? 0 : clock->next;
}

static void push(struct R5Clock *clock, uint64_t tick, int64_t ns)
{
    clock->tick[clock->next] = tick;
    clock->ns[clock->next] = ns;
    clock->next = (clock->next + 1) % R5CLOCK_HISTORY;
    if (clock->count < R5CLOCK_HISTORY) {
        clock->count++;
    }
}

static void clear(struct R5Clock *clock)
{
    clock->count = 0;
    clock->next = 0;
}

void R5Clock_init(struct R5Clock *clock, uint32_t tickHz)
{
    clock->nominalNsPerTick = 1e9 / (double)(tickHz ? tickHz : 1);
    clock->nsPerTick = clock->nominalNsPerTick;
    clock->arrivals = false;
    clock->steps = 0;
    clear(clock);
}

bool R5Clock_isValid(const struct R5Clock *clock)
{
    return clock->count > 0;
}

// Fits the rate from the oldest to the newest pair, if far enough apart
static void fitRate(struct R5Clock *clock)
{
    int o = oldestIndex(clock), n = newestIndex(clock);
    double ticks = (double)(int64_t)(clock->tick[n] - clock->tick[o]);
    double ns = (double)(clock->ns[n] - clock->ns[o]);
    if (ticks * clock->nominalNsPerTick < R5CLOCK_MIN_FIT_S * 1e9) {
        return;
    }
    double rate = ns / ticks;
    double error = rate / clock->nominalNsPerTick - 1.0;
    if (error > -R5CLOCK_MAX_RATE_ERROR && error < R5CLOCK_MAX_RATE_ERROR) {
        clock->nsPerTick = rate;
    }
}

void R5Clock_addPair(struct R5Clock *clock, uint64_t tick, int64_t realtimeNs)
{
    if (clock->arrivals) {
        clear(clock);
        clock->arrivals = false;
    }
    if (clock->count > 0) {
        // Allowed error: a step threshold plus the largest believable rate
        // error over the time since the newest pair (an unfitted 0.2 % rate
        // error is 2 ms after 1 s, and must not look like a step)
        int64_t predicted = R5Clock_toRealtimeNs(clock, tick);
        int64_t error = realtimeNs - predicted;
        double since = (double)(int64_t)(tick - clock->tick[newestIndex(clock)]) * clock->nsPerTick;
        int64_t allowed = R5CLOCK_STEP_NS + (int64_t)(R5CLOCK_MAX_RATE_ERROR * (since < 0 ? -since : since));
        if (error > allowed || error < -allowed) {
            clear(clock);   // CLOCK_REALTIME was stepped: start the fit again (rate kept)
            clock->steps++;
        }
    }
    push(clock, tick, realtimeNs);
    fitRate(clock);
}

void R5Clock_addArrival(struct R5Clock *clock, uint64_t endTick, int64_t realtimeNs)
{
    if (!clock->arrivals) {
        clear(clock);
        clock->arrivals = true;
        clock->nsPerTick = clock->nominalNsPerTick;
    }
    if (clock->count > 0) {
        int64_t predicted = R5Clock_toRealtimeNs(clock, endTick);
        if (realtimeNs < predicted - R5CLOCK_STEP_NS) {
            clear(clock);   // arrived "before" it ended: the clock was stepped back
            clock->steps++;
        }
    }
    push(clock, endTick, realtimeNs);
}

int64_t R5Clock_toRealtimeNs(const struct R5Clock *clock, uint64_t tick)
{
    if (clock->count == 0) {
        return 0;
    }
    if (!clock->arrivals) {
        int n = newestIndex(clock);
        double dt = (double)(int64_t)(tick - clock->tick[n]) * clock->nsPerTick;
        return clock->ns[n] + (int64_t)dt;
    }
    // Arrivals: the smallest delay (arrival minus end) over the history, at the nominal rate
    int64_t best = 0;
    bool have = false;
    int n = newestIndex(clock);
    for (int i = 0; i < clock->count; i++) {
        double back = (double)(int64_t)(clock->tick[n] - clock->tick[i]) * clock->nsPerTick;
        int64_t atNewest = clock->ns[i] + (int64_t)back;   // this arrival, carried to the newest tick
        if (!have || atNewest < best) {
            best = atNewest;
            have = true;
        }
    }
    double dt = (double)(int64_t)(tick - clock->tick[n]) * clock->nsPerTick;
    return best + (int64_t)dt;
}

double R5Clock_ticksToNs(const struct R5Clock *clock, double ticks)
{
    return ticks * clock->nsPerTick;
}

uint64_t R5Clock_extendTick(uint32_t low, uint64_t nearTick)
{
    uint64_t candidate = (nearTick & ~(uint64_t)0xFFFFFFFFu) | low;
    if (candidate > nearTick && candidate - nearTick > 0x80000000u && candidate >= 0x100000000ull) {
        candidate -= 0x100000000ull;
    } else if (candidate < nearTick && nearTick - candidate > 0x80000000u) {
        candidate += 0x100000000ull;
    }
    return candidate;
}
