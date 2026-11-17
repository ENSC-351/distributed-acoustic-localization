// Entry point for the master executable: collects sound directions from the
// nodes and works out where the sound came from
// Usage: master [port]

#include "common/protocol.h"
#include "common/shutdown.h"
#include "master/eventBuffer.h"
#include "master/localizer.h"
#include "master/netServer.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DECIMAL 10

static bool parseUint16(const char *pText, uint16_t *pValue);

int main(int argc, char *argv[])
{
	// The port is optional: "master" uses PROTO_PORT, "master 35199" uses 35199
	uint16_t port = PROTO_PORT;
	bool isValid = (argc == 1) || (argc == 2 && parseUint16(argv[1], &port));
	if (!isValid) {
		fprintf(stderr, "Usage: %s [port]\n", argv[0]);
		fprintf(stderr, "  port defaults to %d\n", PROTO_PORT);
		return EXIT_FAILURE;
	}

	// Shutdown_init() must come first so that every thread started after it
	// has SIGINT and SIGTERM blocked
	Shutdown_init();
	// The event buffer must exist before the threads that use it start
	//EventBuffer_init(); //this does nothing yet
	NetServer_init(port);

	// Wait for Ctrl+C or SIGTERM
	Shutdown_waitForRequest();

	// Stop in the reverse order, so no module is still using one that has
	// already stopped (the server stops adding events before the buffer goes)
	NetServer_cleanup();
	//EventBuffer_cleanup();
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
	// reject empty text, junk, values that are too big
	bool isValid = (pEnd != pText && *pEnd == '\0' && errno == 0 && value <= UINT16_MAX);
	if (isValid) {
		*pValue = (uint16_t)value;
	}
	return isValid;
}
