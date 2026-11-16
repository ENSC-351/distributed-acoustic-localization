// dspLink.h
// DSP link module: how the networking gets results from the DSP.
//
// PLACEHOLDER: the real DSP to networking interface isn't decided yet.
// fakeDsp.c implements this with made-up events so the networking can be
// tested on its own. The networking only touches the DSP through these
// functions, so swapping in the real one shouldn't change netClient.c.
//
// Only the networking thread calls these (apart from init and cleanup).

#ifndef DSP_LINK_H
#define DSP_LINK_H

#include "common/protocol.h"
#include <stdbool.h>
#include <stdint.h>

void DspLink_init(void);
void DspLink_cleanup(void);

// Number of microphones on this node, for HELLO
uint8_t DspLink_getMicCount(void);

// Take the oldest event the DSP has found. Returns false if there are none.
// Never blocks. Leaves t_send_ns for the networking to fill in.
bool DspLink_takeEvent(struct proto_event *pEvent);

// True if the DSP has made progress since the last call
bool DspLink_isDspAlive(void);

// Events the DSP has had to drop since it started
uint32_t DspLink_getDropCount(void);

#endif
