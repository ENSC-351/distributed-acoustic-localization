// eventBuffer.c
// Event buffer module: a fixed size ring of events, shared between the
// network server thread and the localization thread. See
// master/include/master/eventBuffer.h for what each function does.

#include "master/eventBuffer.h"
#include "common/timing.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <stdatomic.h>

#define EVENT_BUFFER_CAPACITY 64
#define EVENT_BUFFER_CLOCK CLOCK_REALTIME //using CLOCK_MONOTONIC might be better

#define NS_PER_MS 1000000LL
#define NS_PER_S  1000000000LL

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static EventBuffer_entry_t eventBuffer[EVENT_BUFFER_CAPACITY];  // the ring of entries
static size_t oldestIndex;  // index of the oldest entry in the buffer
static size_t numEntries;   // how many entries are currently in the buffer
static _Atomic uint32_t dropCount;  // how many entries have been dropped

// lock protects all the state above. notEmpty lets EventBuffer_waitAndTake()
// sleep until EventBuffer_add() signals that an entry has arrived.
static pthread_cond_t notEmpty;

void EventBuffer_init(void)
{
	pthread_condattr_t attr;
	int result = pthread_condattr_init(&attr);
	if (result == 0) {
		result = pthread_condattr_setclock(&attr, EVENT_BUFFER_CLOCK);
	}
	if (result == 0) {
		result = pthread_cond_init(&notEmpty, &attr);
	}
	pthread_condattr_destroy(&attr);  // the condition keeps its own copy
	if (result != 0) {
		fprintf(stderr, "EventBuffer: can't set up the condition variable: %s\n",
		        strerror(result));
		exit(EXIT_FAILURE);
	}

	// Start empty
	pthread_mutex_lock(&lock);
	oldestIndex = 0;
	numEntries = 0;
	dropCount = 0;
	pthread_mutex_unlock(&lock);
}

void EventBuffer_cleanup(void)
{
	// only call this once no thread can still be in EventBuffer_add() or
	// waiting in EventBuffer_waitAndTake(): destroying a condition variable
	// that a thread is waiting on is undefined behaviour
	pthread_mutex_lock(&lock);
	uint32_t numDropped = dropCount;
	size_t numLeft = numEntries;
	pthread_mutex_unlock(&lock);
	printf("EventBuffer: %u events dropped, %zu never taken.\n",
	       (unsigned)numDropped, numLeft);
	// lock was set up with PTHREAD_MUTEX_INITIALIZER, so it deosnt need to be destroyed.
	pthread_cond_destroy(&notEmpty); //notEmpty was set up by pthread_cond_init(), so it needs to be destroyed
}

void EventBuffer_add(const EventBuffer_entry_t *pEntry)
{
	assert(pEntry != NULL);

	pthread_mutex_lock(&lock);
	if (numEntries == EVENT_BUFFER_CAPACITY) {
		oldestIndex = (oldestIndex + 1) % EVENT_BUFFER_CAPACITY; //when takes removes oldest entry, oldest entry changes
		numEntries--; //when takes removes oldest entry, number of entries decreases
		dropCount++;
	}
	size_t nextIndex = (oldestIndex + numEntries) % EVENT_BUFFER_CAPACITY;
	eventBuffer[nextIndex] = *pEntry;
	numEntries++;
	pthread_cond_signal(&notEmpty);
	pthread_mutex_unlock(&lock);
}

// Copy the oldest entry into *pEntry and remove it from the ring. Returns
// false (and zeroes *pEntry) if the ring is empty.
// The caller must already hold lock.
static bool takeOldestLocked(EventBuffer_entry_t *pEntry)
{
	if (numEntries == 0) {
		*pEntry = (EventBuffer_entry_t){0};  // avoid uninitialized memory
		return false;
	}
	*pEntry = eventBuffer[oldestIndex];
	oldestIndex = (oldestIndex + 1) % EVENT_BUFFER_CAPACITY;
	numEntries--;
	return true;
}

bool EventBuffer_take(EventBuffer_entry_t *pEntry)
{
	assert(pEntry != NULL);
	// EventBuffer_Take never waits: if the ring is empty, return false right away
	pthread_mutex_lock(&lock);
	bool isTaken = takeOldestLocked(pEntry);
	pthread_mutex_unlock(&lock);
	return isTaken;
}

bool EventBuffer_waitAndTake(EventBuffer_entry_t *pEntry, long long timeoutMs)
{
	assert(pEntry != NULL);
	assert(timeoutMs >= 0);

	// 1. pthread_cond_timedwait() takes an absolute deadline, not a duration,
	//    so it's now + timeoutMs on the condition's own clock
	struct timespec deadline;
	clock_gettime(EVENT_BUFFER_CLOCK, &deadline);
	deadline.tv_sec += timeoutMs / MS_PER_SECOND;
	deadline.tv_nsec += (timeoutMs % MS_PER_SECOND) * NS_PER_MS;
	if (deadline.tv_nsec >= NS_PER_S) {  // carry, so tv_nsec stays < 1e9
		deadline.tv_sec++;
		deadline.tv_nsec -= NS_PER_S;
	}

	// 2. Wait in a loop: a wakeup only means "check again". It can be
	//    spurious, or another thread may have taken the entry first.
	//    Stop on ETIMEDOUT, or on any other error so it can't spin forever.
	pthread_mutex_lock(&lock);
	int result = 0;
	while (numEntries == 0 && result == 0) {
		result = pthread_cond_timedwait(&notEmpty, &lock, &deadline);
	}
	if (result != 0 && result != ETIMEDOUT) {
		fprintf(stderr, "EventBuffer: timed wait failed: %s\n", strerror(result));
	}

	// 3. an entry may have arrived just as the deadline passed, so check
	//    the ring rather than trusting the return code
	bool isTaken = takeOldestLocked(pEntry);
	pthread_mutex_unlock(&lock);
	return isTaken;
}

uint32_t EventBuffer_getDropCount(void)
{
	pthread_mutex_lock(&lock);
	uint32_t numDropped = dropCount;
	pthread_mutex_unlock(&lock);
	return numDropped;
}
