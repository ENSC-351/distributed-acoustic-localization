// Entry point for the node executable: reports sound directions to the master
// Usage: node <node ID> <master IP> [port]

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

    // Shutdown_init() must come first so that every thread that is started after it
    // has SIGINT and SIGTERM blocked
	Shutdown_init();
	DspLink_init();
	NetClient_init(nodeId, argv[2], port);

	// Wait for Ctrl+C or SIGTERM
	Shutdown_waitForRequest();

	// Stop in the reverse order so that no module is still using one that has already stopped
	NetClient_cleanup();
	DspLink_cleanup();
	Shutdown_cleanup();
	return EXIT_SUCCESS;
}

static bool parseArgs(int argc, char *argv[], uint16_t *pNodeId, uint16_t *pPort)
{
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
	errno = 0;
	unsigned long value = strtoul(pText, &pEnd, DECIMAL);
	// reject empty text, junk, values that are too big
	bool isValid = (pEnd != pText && *pEnd == '\0' && errno == 0 && value <= UINT16_MAX);
	if (isValid) {
		*pValue = (uint16_t)value;
	}
	return isValid;
}
