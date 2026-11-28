// localizer.c
// Localizer module: a thread takes each event out of the event buffer and
// hands it on to be fused with the other nodes' reports. Fusion (on
// madden/feature/web-gui) does the grouping and triangulation, so this
// module only moves events from the buffer to it.
//
// Until fusion is merged in, each event is printed, along with how long it
// took to get from the node to the master.

#include "master/localizer.h"
#include "master/eventBuffer.h"
#include "fusion.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// How long the thread waits for an event before checking for shutdown
#define WAIT_PERIOD_MS 100

#define NS_PER_MS 1000000.0

// Thread start/stop state (from netClient.c)
static pthread_t localizerThread;
static bool isRunning = false;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static bool isThreadRunning(void);
static void handleEntry(const EventBuffer_entry_t *pEntry);
static void printEntry(const EventBuffer_entry_t *pEntry);

static void *localizerThreadFn(void *pArg)
{
	(void)pArg;
	// Wait with a timeout instead of forever, so the thread notices
	// Localizer_cleanup() within WAIT_PERIOD_MS even if no events arrive
	while (isThreadRunning()) {
		EventBuffer_entry_t entry;
		if (EventBuffer_waitAndTake(&entry, WAIT_PERIOD_MS)) {
			handleEntry(&entry);
		}
	}
	return NULL;
}

void Localizer_init(void)
{
	// (from netClient.c) only one localizer thread
	pthread_mutex_lock(&lock);
	if (isRunning) {
		pthread_mutex_unlock(&lock);
		fprintf(stderr, "The localizer thread is already running.\n");
		return;
	}
	isRunning = true;
	pthread_mutex_unlock(&lock);

	int result = pthread_create(&localizerThread, NULL, localizerThreadFn, NULL);
	if (result != 0) {
		fprintf(stderr, "Failed to create localizer thread!\n");
		pthread_mutex_lock(&lock);
		isRunning = false;
		pthread_mutex_unlock(&lock);
	}
}

void Localizer_cleanup(void)
{
	pthread_mutex_lock(&lock);
	if (!isRunning) {
		pthread_mutex_unlock(&lock);
		return; // Thread wasn't running
	}
	isRunning = false;
	pthread_mutex_unlock(&lock);

	// The thread never blocks for longer than WAIT_PERIOD_MS, so this
	// returns quickly
	pthread_join(localizerThread, NULL);
	printf("Localizer thread cleaned up successfully. %" PRIu32 " events were dropped by the buffer.\n",
	       EventBuffer_getDropCount());
}

static bool isThreadRunning(void)
{
	pthread_mutex_lock(&lock);
	bool shouldRun = isRunning;
	pthread_mutex_unlock(&lock);
	return shouldRun;
}

// What to do with one event taken from the buffer
static void handleEntry(const EventBuffer_entry_t *pEntry)
{
	printEntry(pEntry);

	// Fusion groups the nodes' reports and triangulates; it has its own
	// mutex, so no extra locking is needed
	Fusion_addEvent(pEntry->nodeId, pEntry->event.theta,
	                pEntry->event.confidence, pEntry->event.flags);
}

static void printEntry(const EventBuffer_entry_t *pEntry)
{
	const struct proto_event *pEvent = &pEntry->event;

	// Both times are uint64_t: if the node's clock is slightly ahead of the
	// master's, the unsigned difference would wrap to a huge number, so cast
	// each side to signed first. A negative value is only clock offset, not
	// an error.
	int64_t latencyNs = (int64_t)pEntry->recvTimeNs - (int64_t)pEvent->t_send_ns;

	printf("Localizer: node %u event %" PRIu32 ": theta %.1f deg, confidence %.2f, "
	       "freq %.0f Hz, latency %.2f ms\n",
	       pEntry->nodeId, pEvent->event_id, pEvent->theta, pEvent->confidence,
	       pEvent->dominant_freq_hz, latencyNs / NS_PER_MS);
}
