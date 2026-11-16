// netClient.c
// Network client module: connects to the master, says HELLO, then sends
// EVENTs and HEARTBEATs until the connection drops or the program shuts down.

#include "node/netClient.h"
#include "node/dspLink.h"
#include "common/framing.h"
#include "common/protocol.h"
#include "common/shutdown.h"
#include "common/timing.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timex.h>
#include <unistd.h>

// How long the thread waits on the socket before checking the DSP for
// events, so this is also the most an event waits before being sent
#define POLL_PERIOD_MS 20

// Placeholder until the build embeds part of the git hash
#define BUILD_ID 0

static pthread_t netClientThread;
static bool isRunning = false;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// Set before the thread starts, then only used by the thread
static struct sockaddr_in masterAddress;
static uint16_t myNodeId = 0;
static uint32_t nextSeq = 0;
static long long heartbeatPeriodMs = PROTO_HEARTBEAT_MS;

static bool isThreadRunning(void);

static int connectToMaster(void);
static bool setSocketOptions(int socketFd);
static int waitForConnect(int socketFd);

static bool sayHello(int socketFd, Framing_reader_t *pReader);
static bool handleHelloAck(const struct proto_header *pHeader, const uint8_t *pPayload);
static const char *describeHelloStatus(uint8_t status);

static void runSession(int socketFd, Framing_reader_t *pReader);
static bool readFromMaster(int socketFd, Framing_reader_t *pReader);
static bool handleMessagesFromMaster(Framing_reader_t *pReader);
static void handleBye(const uint8_t *pPayload, size_t payloadLen);
static bool sendDspEvents(int socketFd, long long *pLastSendMs);
static bool sendHeartbeat(int socketFd);
static void fillClockStatus(struct proto_heartbeat *pHeartbeat);
static bool sendBye(int socketFd, uint8_t reason);
static bool sendMessage(int socketFd, uint8_t *pMessage, uint8_t msgType, size_t payloadLen);
static bool reportFramingError(Framing_result_t result);

static void waitToReconnect(void);
static void discardDspEvents(void);

static void *netClientThreadFn(void *pArg)
{
	(void)pArg;
	while (isThreadRunning()) {
		int socketFd = connectToMaster();
		if (socketFd >= 0) {
			Framing_reader_t reader;
			Framing_initReader(&reader);
			if (sayHello(socketFd, &reader)) {
				runSession(socketFd, &reader);
			}
			close(socketFd);
		}
		waitToReconnect();
	}
	return NULL;
}

void NetClient_init(uint16_t nodeId, const char *pMasterIp, uint16_t port)
{
	assert(pMasterIp != NULL);
	pthread_mutex_lock(&lock);
	if (isRunning) {
		pthread_mutex_unlock(&lock);
		fprintf(stderr, "The netClient thread is already running.\n");
		return;
	}
	isRunning = true;
	pthread_mutex_unlock(&lock);

	myNodeId = nodeId;
	masterAddress = (struct sockaddr_in){0};
	masterAddress.sin_family = AF_INET;
	masterAddress.sin_port = htons(port);
	if (inet_pton(AF_INET, pMasterIp, &masterAddress.sin_addr) != 1) {
		fprintf(stderr, "'%s' is not a valid IPv4 address.\n", pMasterIp);
		exit(EXIT_FAILURE);
	}

	int result = pthread_create(&netClientThread, NULL, netClientThreadFn, NULL);
	if (result != 0) {
		fprintf(stderr, "Failed to create netClient thread!\n");
		pthread_mutex_lock(&lock);
		isRunning = false;
		pthread_mutex_unlock(&lock);
	}
}

void NetClient_cleanup(void)
{
	// Signal the netClient thread to stop
	pthread_mutex_lock(&lock);
	if (!isRunning) {
		pthread_mutex_unlock(&lock);
		return; // Thread wasn't running
	}
	isRunning = false;
	pthread_mutex_unlock(&lock);

	// The thread never blocks for long (POLL_PERIOD_MS, or PROTO_RECONNECT_MS
	// while connecting), so it notices quickly and sends BYE on its way out
	pthread_join(netClientThread, NULL);
	printf("NetClient thread cleaned up successfully.\n");
}

static bool isThreadRunning(void)
{
	pthread_mutex_lock(&lock);
	bool shouldRun = isRunning;
	pthread_mutex_unlock(&lock);
	return shouldRun;
}

// Returns the connected socket, or -1 if it couldn't connect
static int connectToMaster(void)
{
	int socketFd = socket(AF_INET, SOCK_STREAM, 0);
	if (socketFd < 0) {
		perror("NetClient socket");
		return -1;
	}

	bool isConnected = false;
	if (setSocketOptions(socketFd)) {
		// A blocking connect() to a board that's powered off waits around
		// 2 minutes for TCP to give up. Connecting in non-blocking mode lets
		// us stop waiting after PROTO_RECONNECT_MS instead.
		int flags = fcntl(socketFd, F_GETFL);
		fcntl(socketFd, F_SETFL, flags | O_NONBLOCK);

		int error = 0;
		if (connect(socketFd, (struct sockaddr *)&masterAddress, sizeof(masterAddress)) < 0) {
			error = (errno == EINPROGRESS) ? waitForConnect(socketFd) : errno;
		}
		isConnected = (error == 0);
		if (!isConnected) {
			fprintf(stderr, "Can't connect to master: %s\n", strerror(error));
		}

		// Back to blocking, so a send never fails with EAGAIN. TCP_USER_TIMEOUT
		// stops a send from blocking for long if the master disappears.
		fcntl(socketFd, F_SETFL, flags);
	}

	if (!isConnected) {
		close(socketFd);
		socketFd = -1;
	}
	return socketFd;
}

static bool setSocketOptions(int socketFd)
{
	// Send each small message right away, instead of holding it back to
	// merge with the next one (Nagle's algorithm)
	int isNoDelay = 1;

	// Give up on the connection when sent data goes unacknowledged for this
	// long, instead of TCP's default of around 15 minutes. HEARTBEATs mean
	// there's always something recent in flight, so a dead master is
	// noticed within about PROTO_HEARTBEAT_MS + PROTO_TIMEOUT_MS.
	unsigned int userTimeoutMs = PROTO_TIMEOUT_MS;

	bool isOk = (setsockopt(socketFd, IPPROTO_TCP, TCP_NODELAY,
	                        &isNoDelay, sizeof(isNoDelay)) == 0)
	            && (setsockopt(socketFd, IPPROTO_TCP, TCP_USER_TIMEOUT,
	                           &userTimeoutMs, sizeof(userTimeoutMs)) == 0);
	if (!isOk) {
		perror("NetClient setsockopt");
	}
	return isOk;
}

// Wait for a non-blocking connect() to finish. Returns 0 if it connected,
// otherwise the errno value saying why not.
static int waitForConnect(int socketFd)
{
	// The socket becomes writable once the connection succeeds or fails
	struct pollfd pollEntry = {.fd = socketFd, .events = POLLOUT};
	int numReady = poll(&pollEntry, 1, PROTO_RECONNECT_MS);

	int error = ETIMEDOUT;
	if (numReady < 0) {
		error = errno;
	} else if (numReady > 0) {
		socklen_t errorLen = sizeof(error);
		getsockopt(socketFd, SOL_SOCKET, SO_ERROR, &error, &errorLen);
	}
	return error;
}

// Send HELLO and wait for the HELLO_ACK. Returns true if the master
// accepted this node.
static bool sayHello(int socketFd, Framing_reader_t *pReader)
{
	uint8_t message[PROTO_HEADER_LEN + PROTO_HELLO_LEN];
	struct proto_hello hello = {
		.mic_count = DspLink_getMicCount(),
		.build_id = BUILD_ID,
	};
	size_t payloadLen = proto_pack_hello(message + PROTO_HEADER_LEN, &hello);
	bool isConnected = sendMessage(socketFd, message, PROTO_MSG_HELLO, payloadLen);

	struct proto_header header;
	const uint8_t *pPayload = NULL;
	Framing_result_t result = FRAMING_NEED_MORE_BYTES;
	long long deadlineMs = Timing_getMonotonicMs() + PROTO_TIMEOUT_MS;
	while (isConnected
	       && result == FRAMING_NEED_MORE_BYTES
	       && isThreadRunning()
	       && Timing_getMonotonicMs() < deadlineMs)
	{
		isConnected = readFromMaster(socketFd, pReader);
		result = Framing_getMessage(pReader, &header, &pPayload);
	}

	bool isAccepted = false;
	if (result == FRAMING_MESSAGE_READY) {
		isAccepted = handleHelloAck(&header, pPayload);
	} else if (result != FRAMING_NEED_MORE_BYTES) {
		reportFramingError(result);
	} else if (isConnected && isThreadRunning()) {
		fprintf(stderr, "Master didn't answer HELLO within %d ms.\n", PROTO_TIMEOUT_MS);
	}
	return isAccepted;
}

static bool handleHelloAck(const struct proto_header *pHeader, const uint8_t *pPayload)
{
	struct proto_hello_ack ack;
	if (pHeader->msg_type != PROTO_MSG_HELLO_ACK
	    || proto_unpack_hello_ack(pPayload, pHeader->payload_len, &ack) != PROTO_OK)
	{
		fprintf(stderr, "Master answered HELLO with message type 0x%02x, not HELLO_ACK.\n",
		        pHeader->msg_type);
		return false;
	}

	bool isAccepted = (ack.status == PROTO_HELLO_OK);
	if (isAccepted) {
		// 0 would mean a HEARTBEAT on every loop, so treat it as "use the default"
		heartbeatPeriodMs = (ack.heartbeat_ms > 0) ? ack.heartbeat_ms : PROTO_HEARTBEAT_MS;
		printf("Connected to master as node %u (heartbeat every %lld ms).\n",
		       myNodeId, heartbeatPeriodMs);
	} else {
		fprintf(stderr, "Master rejected HELLO: %s.\n", describeHelloStatus(ack.status));
	}
	return isAccepted;
}

static const char *describeHelloStatus(uint8_t status)
{
	const char *pDescription = "unknown status";
	switch (status) {
	case PROTO_HELLO_OK:
		pDescription = "OK";
		break;
	case PROTO_HELLO_UNKNOWN_ID:
		pDescription = "the master doesn't know this node ID";
		break;
	case PROTO_HELLO_DUPLICATE:
		pDescription = "another node already has this ID";
		break;
	case PROTO_HELLO_VERSION:
		pDescription = "protocol version mismatch";
		break;
	default:
		// A newer master might send a status this build doesn't know about
		break;
	}
	return pDescription;
}

// Send events and heartbeats until the connection drops or a shutdown is
// requested
static void runSession(int socketFd, Framing_reader_t *pReader)
{
	// Anything the DSP found while connecting is already too late to be useful
	discardDspEvents();

	long long lastSendMs = Timing_getMonotonicMs();
	bool isConnected = true;
	while (isConnected && isThreadRunning()) {
		isConnected = readFromMaster(socketFd, pReader)
		              && handleMessagesFromMaster(pReader)
		              && sendDspEvents(socketFd, &lastSendMs);

		bool isHeartbeatDue = (Timing_getMonotonicMs() - lastSendMs >= heartbeatPeriodMs);
		if (isConnected && isHeartbeatDue) {
			isConnected = sendHeartbeat(socketFd);
			lastSendMs = Timing_getMonotonicMs();
		}
	}

	if (isConnected) {
		// Still connected, so we're leaving because of a shutdown. Telling
		// the master means it doesn't have to wait for a timeout.
		sendBye(socketFd, PROTO_BYE_SHUTDOWN);
	}
	printf("Disconnected from master.\n");
}

// Wait up to POLL_PERIOD_MS for bytes from the master, and read them if any
// arrive. Returns false if the connection is gone.
static bool readFromMaster(int socketFd, Framing_reader_t *pReader)
{
	struct pollfd pollEntry = {.fd = socketFd, .events = POLLIN};
	int numReady = poll(&pollEntry, 1, POLL_PERIOD_MS);

	bool isConnected = true;
	if (numReady < 0) {
		isConnected = (errno == EINTR);
	} else if (numReady > 0) {
		// Also covers POLLHUP and POLLERR: recv() then reports what went wrong
		ssize_t numBytesRead = Framing_readFromSocket(pReader, socketFd);
		if (numBytesRead == 0) {
			fprintf(stderr, "Master closed the connection.\n");
			isConnected = false;
		} else if (numBytesRead < 0 && errno != EINTR) {
			perror("NetClient recv");
			isConnected = false;
		}
	}
	return isConnected;
}

// Handle every complete message the master has sent. Returns false if the
// connection should be closed.
static bool handleMessagesFromMaster(Framing_reader_t *pReader)
{
	struct proto_header header;
	const uint8_t *pPayload = NULL;
	bool isConnected = true;
	Framing_result_t result = Framing_getMessage(pReader, &header, &pPayload);
	while (isConnected && result == FRAMING_MESSAGE_READY) {
		switch (header.msg_type) {
		case PROTO_MSG_BYE:
			handleBye(pPayload, header.payload_len);
			isConnected = false;
			break;
		default:
			// Skip types this build doesn't know, so the master can add
			// commands without breaking older nodes
			fprintf(stderr, "Ignoring message type 0x%02x from master.\n", header.msg_type);
			break;
		}
		result = Framing_getMessage(pReader, &header, &pPayload);
	}

	if (isConnected && result != FRAMING_NEED_MORE_BYTES) {
		isConnected = reportFramingError(result);
	}
	return isConnected;
}

static void handleBye(const uint8_t *pPayload, size_t payloadLen)
{
	struct proto_bye bye;
	if (proto_unpack_bye(pPayload, payloadLen, &bye) != PROTO_OK) {
		fprintf(stderr, "Master said BYE.\n");
	} else if (bye.reason == PROTO_BYE_REPLACED) {
		// The master replaces an old connection when a node with the same ID
		// says HELLO, to clean up after a node that rebooted. A node that
		// really rebooted is gone and never sees this BYE, so getting it
		// means another running node has our ID. Reconnecting would knock
		// that node off in turn, and the two would swap places every second.
		fprintf(stderr, "Another node connected as node %u, so this one is shutting down. "
		                "Check that every node has its own ID.\n", myNodeId);
		Shutdown_request();
	} else {
		fprintf(stderr, "Master said BYE (reason %u).\n", bye.reason);
	}
}

static bool sendDspEvents(int socketFd, long long *pLastSendMs)
{
	struct proto_event event;
	bool isConnected = true;
	while (isConnected && DspLink_takeEvent(&event)) {
		event.t_send_ns = Timing_getRealtimeNs();
		uint8_t message[PROTO_HEADER_LEN + PROTO_EVENT_LEN];
		size_t payloadLen = proto_pack_event(message + PROTO_HEADER_LEN, &event);
		isConnected = sendMessage(socketFd, message, PROTO_MSG_EVENT, payloadLen);
		*pLastSendMs = Timing_getMonotonicMs();
	}
	return isConnected;
}

static bool sendHeartbeat(int socketFd)
{
	struct proto_heartbeat heartbeat = {
		.t_send_ns = Timing_getRealtimeNs(),
		.dsp_alive = DspLink_isDspAlive(),
		.ring_drops = DspLink_getDropCount(),
	};
	fillClockStatus(&heartbeat);

	uint8_t message[PROTO_HEADER_LEN + PROTO_HEARTBEAT_LEN];
	size_t payloadLen = proto_pack_heartbeat(message + PROTO_HEADER_LEN, &heartbeat);
	return sendMessage(socketFd, message, PROTO_MSG_HEARTBEAT, payloadLen);
}

// chrony keeps the kernel's clock status up to date, so adjtimex() tells us
// how well synced we are without having to talk to chrony
static void fillClockStatus(struct proto_heartbeat *pHeartbeat)
{
	// modes = 0 makes adjtimex() read only, so it doesn't need root
	struct timex clockInfo = {0};
	int clockState = adjtimex(&clockInfo);
	pHeartbeat->clock_synced = (clockState >= 0 && clockState != TIME_ERROR);
	pHeartbeat->clock_err_us = (uint32_t)clockInfo.esterror;
}

static bool sendBye(int socketFd, uint8_t reason)
{
	uint8_t message[PROTO_HEADER_LEN + PROTO_BYE_LEN];
	struct proto_bye bye = {.reason = reason};
	size_t payloadLen = proto_pack_bye(message + PROTO_HEADER_LEN, &bye);
	return sendMessage(socketFd, message, PROTO_MSG_BYE, payloadLen);
}

// Send a message whose payload is already packed at pMessage + PROTO_HEADER_LEN
static bool sendMessage(int socketFd, uint8_t *pMessage, uint8_t msgType, size_t payloadLen)
{
	struct proto_header header = proto_make_header(msgType, (uint16_t)payloadLen,
	                                               myNodeId, nextSeq);
	nextSeq++;
	bool isSent = Framing_sendMessage(socketFd, pMessage, &header);
	if (!isSent) {
		perror("NetClient send");
	}
	return isSent;
}

// Every framing error means the stream can't be trusted, so always returns
// false (the connection should close)
static bool reportFramingError(Framing_result_t result)
{
	switch (result) {
	case FRAMING_BAD_MAGIC:
		fprintf(stderr, "Bad magic from master, the stream is out of sync.\n");
		break;
	case FRAMING_BAD_VERSION:
		fprintf(stderr, "Master speaks a different protocol version.\n");
		break;
	case FRAMING_BAD_LENGTH:
		fprintf(stderr, "Master sent a payload longer than %d bytes.\n", PROTO_MAX_PAYLOAD);
		break;
	default:
		assert(false);
	}
	return false;
}

// Sleep for PROTO_RECONNECT_MS, in short steps so a shutdown isn't held up
static void waitToReconnect(void)
{
	long long endMs = Timing_getMonotonicMs() + PROTO_RECONNECT_MS;
	while (isThreadRunning() && Timing_getMonotonicMs() < endMs) {
		// Keep emptying the DSP's queue, or it fills up and the DSP starts
		// counting drops that aren't really the DSP's fault
		discardDspEvents();
		Timing_sleepForMs(POLL_PERIOD_MS);
	}
}

static void discardDspEvents(void)
{
	struct proto_event event;
	while (DspLink_takeEvent(&event)) {
		// Thrown away: there's no master to send it to
	}
}
