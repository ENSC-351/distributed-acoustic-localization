// fakeDsp.c
// Stand-in for the DSP until we agree on the DSP to networking interface.
// Makes up one event every FAKE_EVENT_PERIOD_MS (with theta stepping around
// the circle), so the networking can be tested without any microphones.
// None of the values mean anything.

#include "node/dspLink.h"
#include "common/timing.h"
#include <assert.h>

// Longer than the heartbeat period, so HEARTBEATs show up between EVENTs
#define FAKE_EVENT_PERIOD_MS 2500
#define FAKE_MIC_COUNT 2
#define FAKE_CONFIDENCE 1.0f
#define FAKE_FREQUENCY_HZ 1000.0f
// Degrees are just a guess, the units are up to the DSP side
#define THETA_STEP_DEG 15.0f
#define FULL_CIRCLE_DEG 360.0f

static long long nextEventMs = 0;
static uint32_t nextEventId = 0;
static float thetaDeg = 0.0f;

void DspLink_init(void)
{
	nextEventMs = Timing_getMonotonicMs() + FAKE_EVENT_PERIOD_MS;
}

void DspLink_cleanup(void)
{
}

uint8_t DspLink_getMicCount(void)
{
	return FAKE_MIC_COUNT;
}

bool DspLink_takeEvent(struct proto_event *pEvent)
{
	assert(pEvent != NULL);
	long long nowMs = Timing_getMonotonicMs();
	bool hasEvent = (nowMs >= nextEventMs);
	if (hasEvent) {
		// Fields left out (phi, flags, and t_send_ns) are zeroed. A flags of
		// 0 marks phi as invalid, like a node with 2 mics.
		*pEvent = (struct proto_event){
			.t_event_ns = Timing_getRealtimeNs(),
			.theta = thetaDeg,
			.confidence = FAKE_CONFIDENCE,
			.dominant_freq_hz = FAKE_FREQUENCY_HZ,
			.event_id = nextEventId,
		};
		nextEventId++;
		thetaDeg += THETA_STEP_DEG;
		if (thetaDeg >= FULL_CIRCLE_DEG) {
			thetaDeg -= FULL_CIRCLE_DEG;
		}
		// Counted from now instead of from the last event, so a caller that
		// hasn't checked in a while gets one event instead of a burst
		nextEventMs = nowMs + FAKE_EVENT_PERIOD_MS;
	}
	return hasEvent;
}

bool DspLink_isDspAlive(void)
{
	return true;
}

uint32_t DspLink_getDropCount(void)
{
	return 0;
}
