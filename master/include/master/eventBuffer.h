// eventBuffer.h
// Event buffer module: holds the EVENTs the network server receives until the
// localizer uses them.
//
// PLACEHOLDER: netServer.c already calls EventBuffer_add(), but the real buffer
// (and the localizer that reads it) hasn't been written yet. eventBuffer.c only
// counts the events for now, so the master builds and runs.

#ifndef EVENT_BUFFER_H
#define EVENT_BUFFER_H

#include "common/protocol.h"
#include <stdint.h>

// One EVENT, plus what the master knows about where and when it came from
typedef struct {
	uint16_t nodeId;
	uint64_t recvTimeNs;   // master's CLOCK_REALTIME when the EVENT arrived
	struct proto_event event;
} EventBuffer_entry_t;

void EventBuffer_init(void);
void EventBuffer_cleanup(void);

// Copies pEntry into the buffer. Called from the network server thread.
void EventBuffer_add(const EventBuffer_entry_t *pEntry);

#endif
