// timing.c
// Timing module: helpers for delaying the calling thread and reading clocks.

#include "common/timing.h"
#include <assert.h>
#include <time.h>

#define NS_PER_MS 1000000LL
#define NS_PER_SECOND 1000000000LL

void Timing_sleepForMs(long long delayInMs)
{
	assert(delayInMs >= 0);
	Timing_sleepForNs(delayInMs * NS_PER_MS);
}

void Timing_sleepForNs(long long delayInNs)
{
	assert(delayInNs >= 0);
	// tv_nsec must stay below one second, so split off the whole seconds
	struct timespec reqDelay = {
		.tv_sec = delayInNs / NS_PER_SECOND,
		.tv_nsec = delayInNs % NS_PER_SECOND,
	};
	nanosleep(&reqDelay, NULL);
}

long long Timing_getMonotonicMs(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec * MS_PER_SECOND) + (now.tv_nsec / NS_PER_MS);
}

uint64_t Timing_getRealtimeNs(void)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	return ((uint64_t)now.tv_sec * NS_PER_SECOND) + (uint64_t)now.tv_nsec;
}
