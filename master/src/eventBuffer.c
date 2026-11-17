// eventBuffer.c
// PLACEHOLDER event buffer: counts the events it's given and drops them, until
// the real buffer and localizer are written.

#include "master/eventBuffer.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long numEventsAdded = 0;

void EventBuffer_init(void)
{
}

void EventBuffer_cleanup(void)
{
	pthread_mutex_lock(&lock);
	unsigned long long numEvents = numEventsAdded;
	pthread_mutex_unlock(&lock);
	printf("EventBuffer received %llu events.\n", numEvents);
}

void EventBuffer_add(const EventBuffer_entry_t *pEntry)
{
	assert(pEntry != NULL);
	// Locked even though only the server thread adds for now, since the real
	// buffer will be read by the localizer thread
	pthread_mutex_lock(&lock);
	numEventsAdded++;
	pthread_mutex_unlock(&lock);
}
