// Entry point for the master executable: collects sound directions from the
// nodes, works out where each sound came from, and serves it to the web GUI.
//
// Usage: master [port] [--demo]
//   port    TCP port the nodes connect to (default PROTO_PORT)
//   --demo  also make up a fake sound every few seconds (no nodes needed)
//
// Data flow:
//   netServer --EventBuffer--> localizer --> fusion --> dashboard --> UDP (web GUI)
//
// Runs until Ctrl+C, SIGTERM, or the web GUI's "stop" command.

#include "common/protocol.h"
#include "common/shutdown.h"
#include "dashboard.h"
#include "demo_source.h"
#include "fusion.h"
#include "layout.h"
#include "master/eventBuffer.h"
#include "master/localizer.h"
#include "master/netServer.h"
#include "udp.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DECIMAL 10

static bool parseUint16(const char *pText, uint16_t *pValue);

int main(int argc, char *argv[])
{
	uint16_t port = PROTO_PORT;
	bool isDemo = false;
	bool isValid = true;
	bool hasPort = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--demo") == 0) {
			isDemo = true;
		} else if (!hasPort && parseUint16(argv[i], &port)) {
			hasPort = true;
		} else {
			isValid = false;
		}
	}
	if (!isValid) {
		fprintf(stderr, "Usage: %s [port] [--demo]\n", argv[0]);
		fprintf(stderr, "  port defaults to %d\n", PROTO_PORT);
		return EXIT_FAILURE;
	}

	// Shutdown_init() must come first so that every thread started after it
	// has SIGINT and SIGTERM blocked
	Shutdown_init();

	Dashboard_init(isDemo);
	for (int i = 0; i < LAYOUT_NUM_NODES; i++) {
		const LayoutNode *node = Layout_getByIndex(i);
		Dashboard_setNodePose(node->id, node->x, node->y, node->headingDeg);
	}
	Fusion_init();
	// The event buffer must exist before the threads that use it start, and
	// the localizer takes from it, so start it before events can arrive
	EventBuffer_init();
	Localizer_init();
	NetServer_init(port);
	UDP_init();               // web GUI; its "stop" command calls Shutdown_request()
	if (isDemo) {
		DemoSource_init();
	}
	printf("Master running%s: nodes on TCP port %u, web GUI data on UDP port %d. Ctrl+C to stop.\n",
	       isDemo ? " with demo data" : "", port, UDP_PORT);

	Shutdown_waitForRequest();
	printf("Master stopping.\n");

	// Stop in the reverse order, so no module is still using one that has
	// already stopped (the server stops adding events before the buffer goes)
	DemoSource_cleanup();
	UDP_cleanup();
	NetServer_cleanup();
	Localizer_cleanup();
	EventBuffer_cleanup();
	Fusion_cleanup();
	Dashboard_cleanup();
	Shutdown_cleanup();
	return EXIT_SUCCESS;
}

// (from node/src/main.c) Unlike atoi(), this rejects junk like "35abc" and
// numbers too big for a port, instead of quietly returning a wrong value
static bool parseUint16(const char *pText, uint16_t *pValue)
{
	char *pEnd = NULL;
	errno = 0;
	unsigned long value = strtoul(pText, &pEnd, DECIMAL);
	bool isValid = (pEnd != pText && *pEnd == '\0' && errno == 0 && value <= UINT16_MAX);
	if (isValid) {
		*pValue = (uint16_t)value;
	}
	return isValid;
}
