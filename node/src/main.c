// Entry point for the node executable, executed via
// `node <id> <master IP> [port]`
// <id> is unique to the node, and can't be 0 (that's PROTO_MASTER_ID).
// [port] may be passed optionally, with a default of PROTO_PORT.

#include "common/protocol.h"
#include "common/shutdown.h"
#include "node/dspLink.h"
#include "node/netClient.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DECIMAL 10

static bool parseArgs(int argc, char *argv[], uint16_t *pNodeId, uint16_t *pPort);
static bool parseUint16(const char *pText, uint16_t *pValue);

int main(int argc, char *argv[])
{
	uint16_t nodeId = 0;
	uint16_t port = PROTO_PORT;

	if (!parseArgs(argc, argv, &nodeId, &port)) {
		fprintf(stderr, "Usage: %s <node ID> <master IP> [port]\n", argv[0]);
		fprintf(stderr, "  node ID is 1 to 65535 (0 is the master)\n");
		fprintf(stderr, "  port defaults to %d\n", PROTO_PORT);
		return EXIT_FAILURE;
	}

	// Shutdown_init() blocks Ctrl+C and SIGTERM, and starts a new thread that waits for them.
	// It must run first because the shutdown thread must be the one to handle
	// a shutdown signal. We don't want a different thread to encounter that
	// signal first, since SIGINT's default action would kill the whole process
	// with no BYE or cleanup.
	Shutdown_init();
	DspLink_init();
	// NetClient_init() starts a networking thread
	NetClient_init(nodeId, argv[2], port);

	// Waits for Ctrl+C, SIGTERM, or a module asking to shut down
	Shutdown_waitForRequest();

	NetClient_cleanup();
	DspLink_cleanup();
	Shutdown_cleanup();
	return EXIT_SUCCESS;
}

static bool parseArgs(int argc, char *argv[], uint16_t *pNodeId,
                      uint16_t *pPort)
{
	// Note intentional use of short-circuiting here, we need to avoid trying to
	// read NULL string, for example if argv[1] == NULL, passing it into strtoul
	// would crash.
	bool isValid = (argc == 3 || argc == 4)
		&& parseUint16(argv[1], pNodeId)
		&& *pNodeId != PROTO_MASTER_ID;
	if (isValid && argc == 4) {
		isValid = parseUint16(argv[3], pPort);
	}
	return isValid;
}

static bool parseUint16(const char *pText, uint16_t *pValue)
{
	char *pEnd = NULL;
	// Set errno to zero before calling strtoul because strtoul's error
	// return values are also valid conversion results and C lib functions
	// do not reset errno to zero even upon success.
	errno = 0;
	unsigned long value = strtoul(pText, &pEnd, DECIMAL);
	// pEnd != pText rejects text with no digits at all
	// *pEnd == '\0' rejects text with junk after the first number
	// errno == 0    rejects numbers that are too big for unsigned long
	// value <= UINT16_MAX rejects numbers that don't fit in 16 bits
	bool isValid = (pEnd != pText && *pEnd == '\0' && errno == 0 && value <= UINT16_MAX);
	if (isValid) {
		*pValue = (uint16_t)value;
	}
	return isValid;
}
