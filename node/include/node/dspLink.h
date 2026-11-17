// dspLink.h
// DSP link module: how the networking gets results from the DSP.
// PLACEHOLDER: we haven't agreed on the DSP to networking interface yet, so
// fakeDsp.c makes up events for now. The networking only touches the DSP
// through these functions, so the real version shouldn't need any changes
// in netClient.c.
// Apart from init and cleanup, only the networking thread calls these.

#ifndef DSP_LINK_H
#define DSP_LINK_H

#include "common/protocol.h"
#include <stdbool.h>
#include <stdint.h>

void DspLink_init(void);
void DspLink_cleanup(void);

// Number of microphones on this node, for HELLO
uint8_t DspLink_getMicCount(void);

// Takes the oldest event the DSP has found. Returns false if there are none.
// It must never block, since the networking checks it every 20 ms in the
// same loop that watches the socket. t_send_ns is left for the networking
// to fill in.
bool DspLink_takeEvent(struct proto_event *pEvent);

// True if the DSP has made progress since the last call
bool DspLink_isDspAlive(void);

// Events the DSP has had to drop since it started (because nobody took them
// in time)
uint32_t DspLink_getDropCount(void);

#endif
