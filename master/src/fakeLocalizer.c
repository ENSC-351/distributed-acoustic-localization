// fakeLocalizer.c
// Stand-in for the localization until it's written. A thread takes each
// event out of the event buffer and prints it, along with how long it took
// to get from the node to the master.
//
// SKELETON: fill in the TODOs. node/src/netClient.c starts and stops its
// thread the same way, so it's a good model.

#include "master/localizer.h"
#include "master/eventBuffer.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

// How long the thread waits for an event before checking for shutdown
#define WAIT_PERIOD_MS 100

// TODO: state for the thread: its pthread_t, an isRunning flag, and a mutex
// protecting the flag

static void *localizerThreadFn(void *pArg)
{
	(void)pArg;
	// TODO: while the thread should keep running, wait up to WAIT_PERIOD_MS
	// for an event and print it if one arrived.
	// Why wait with a timeout instead of forever?
	return NULL;
}

void Localizer_init(void)
{
	// TODO: set isRunning, then start the thread with pthread_create().
	// What should happen if it's already running, or creating it fails?
	(void)localizerThreadFn;
}

void Localizer_cleanup(void)
{
	// TODO: clear isRunning, then pthread_join() the thread
}

// TODO: a helper that prints one EventBuffer_entry_t: node ID, event_id,
// theta, confidence, frequency, and latency in ms.
// Latency = recvTimeNs - event.t_send_ns. Both are uint64_t: what goes
// wrong if the node's clock is slightly ahead of the master's?
