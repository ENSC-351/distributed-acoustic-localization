// eventBuffer.h
// Event buffer module: holds the EVENTs the network server receives until the
// localizer uses them.
//
// The network server adds events from its thread, and the localizer takes
// them from its own thread. If the buffer fills up, the oldest event is
// thrown away, since a newer event is more useful for localization.

#ifndef EVENT_BUFFER_H
#define EVENT_BUFFER_H

#include "common/protocol.h"
#include <stdbool.h>
#include <stdint.h>

// One EVENT, plus what the master knows about where and when it came from
typedef struct {
	uint16_t nodeId;
	uint64_t recvTimeNs;   // master's CLOCK_REALTIME when the EVENT arrived
	struct proto_event event;
} EventBuffer_entry_t;

void EventBuffer_init(void);
void EventBuffer_cleanup(void);

// Copies pEntry into the buffer, throwing away the oldest event if it's full.
// Called from the network server thread.
void EventBuffer_add(const EventBuffer_entry_t *pEntry);

// Take the oldest event. Returns false right away if there are none.
bool EventBuffer_take(EventBuffer_entry_t *pEntry);

// Take the oldest event, waiting up to timeoutMs for one to arrive. Returns
// false if none arrived in time.
bool EventBuffer_waitAndTake(EventBuffer_entry_t *pEntry, long long timeoutMs);

// Events thrown away because the buffer was full
uint32_t EventBuffer_getDropCount(void);

#endif
