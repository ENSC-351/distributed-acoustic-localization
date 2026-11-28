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

// How long each wait on the socket lasts before we check the DSP for events
// and check for a shutdown. This is also the longest an event waits before
// it is sent.
#define POLL_PERIOD_MS 20

// Placeholder until the build puts part of the git hash here
#define BUILD_ID 0

static pthread_t netClientThread;
static bool isRunning = false;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

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

// The whole connection lifecycle: connect, say HELLO, run the session, then
// close and wait before trying again. Every step returns when it fails, so
// any failure ends up at "close and try again".
static void *netClientThreadFn(void *pArg)
{
	(void)pArg;
	while (isThreadRunning()) {
		int socketFd = connectToMaster();
		if (socketFd >= 0) {
			// reader is created for each loop so that it it's data does
			// not persist across node reconnections. A half message left over
			// from a dead connection would be read as the start of the new
			// stream, give FRAMING_BAD_MAGIC, and since nothing ever removes
			// those bytes, every reconnection after that would fail too.
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
	// We only want one net client running per node
	pthread_mutex_lock(&lock);
	if (isRunning) {
		pthread_mutex_unlock(&lock);
		fprintf(stderr, "The netClient thread is already running.\n");
		return;
	}
	isRunning = true;
	pthread_mutex_unlock(&lock);

	// These are written without a mutex which is allowed because they are
	// written before thread creation and will therefore be visible to the
	// new thread. They are not written afterwards so we don't need a lock
	myNodeId = nodeId;
	masterAddress = (struct sockaddr_in){0};
	masterAddress.sin_family = AF_INET;
	masterAddress.sin_port = htons(port);
	// pMasterIp is only validated here.
	if (inet_pton(AF_INET, pMasterIp, &masterAddress.sin_addr) != 1) {
		fprintf(stderr, "'%s' is not a valid IPv4 address.\n", pMasterIp);
		exit(EXIT_FAILURE);
	}

	// Start the thread
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
	pthread_mutex_lock(&lock);
	if (!isRunning) {
		pthread_mutex_unlock(&lock);
		return; // Thread wasn't running
	}
	isRunning = false;
	pthread_mutex_unlock(&lock);

	// We do not need to shutdown() a socket in the same way a UDP server
	// does (like assignment 2) because this thread shouldn't block for
	// long.
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

// Returns the connected socket, or -1 if we couldn't connect.
static int connectToMaster(void)
{
	// Create a TCP socket
	int socketFd = socket(AF_INET, SOCK_STREAM, 0);
	if (socketFd < 0) {
		perror("NetClient socket");
		return -1;
	}

	bool isConnected = false;
	if (setSocketOptions(socketFd)) {
		int flags = fcntl(socketFd, F_GETFL);
		// Non-blocking, so that connect() returns immediately and we decide
		// how long to wait (PROTO_RECONNECT_MS). If the master is off, a
		// blocking connect() retries for around 2 minutes, which would stop
		// the node retrying every second and hold up Ctrl+C just as long.
		fcntl(socketFd, F_SETFL, flags | O_NONBLOCK);
		int error = 0;
		if (connect(socketFd, (struct sockaddr *)&masterAddress, sizeof(masterAddress)) < 0) {
			// Wait for a connection to be made while it is in progress
			// (handshake has started),
			// if it is not in progress, something went wrong.
			// Wait up to 1 second for a connection.
			error = (errno == EINPROGRESS) ? waitForConnect(socketFd) : errno;
		}
		isConnected = (error == 0);
		if (!isConnected) {
			fprintf(stderr, "Can't connect to master: %s\n", strerror(error));
		}
		// Back to blocking, so that send() never fails with EAGAIN (which
		// would make sendAll() spin in a busy loop)
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
	int isNoDelay = 1;
	unsigned int userTimeoutMs = PROTO_TIMEOUT_MS;
	// TCP_NODELAY turns off Nagle's algorithm, it would add some number of
	// milliseconds to our EVENTs.
	bool isOk = (setsockopt(socketFd, IPPROTO_TCP, TCP_NODELAY, &isNoDelay,
	                        sizeof(isNoDelay)) == 0)
		// TCP_USER_TIMEOUT is the amount of time our sent data can go
		// unacknowledged before the kernel declares that the connection has
		// died, by default TCP retries for around 15 minutes. The master never
		// sends anything after HELLO_ACK, so the node only finds out the master
		// is dead by sending. HEARTBEATs make sure that happens at least every
		// heartbeat period, so a dead master is noticed within about 4 s.
		&& (setsockopt(socketFd, IPPROTO_TCP, TCP_USER_TIMEOUT,
		               &userTimeoutMs, sizeof(userTimeoutMs)) == 0);
	if (!isOk) {
		perror("NetClient setsockopt");
	}
	return isOk;
}

// Returns 0 if connection was successful, otherwise it returns errno.
// A connecting socket becomes writeable (POLLOUT) when a handshake finishes
// regardless of success or failure. Poll says when the handshake is done.
static int waitForConnect(int socketFd)
{
	struct pollfd pollEntry = {.fd = socketFd, .events = POLLOUT};
	int numReady = poll(&pollEntry, 1, PROTO_RECONNECT_MS);
	// This is the error if poll returns 0
	int error = ETIMEDOUT;
	if (numReady < 0) {
		error = errno;
	} else if (numReady > 0) {
		socklen_t errorLen = sizeof(error);
		// This tells us how the handshake actually went.
		getsockopt(socketFd, SOL_SOCKET, SO_ERROR, &error, &errorLen);
	}
	return error;
}

// Returns true if the master accepted this node.
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
	// This loop stops at the first of:
	// The connection drops.
	// A full message (or framing error) arrives.
	// A shutdown is requested.
	// PROTO_TIMEOUT_MS passes. Without this, a master that accepts the
	// connection but never answers would make the node wait forever.
	while (isConnected
	       && result == FRAMING_NEED_MORE_BYTES
	       && isThreadRunning()
	       && Timing_getMonotonicMs() < deadlineMs)
	{
		// each readFromMaster() call waits at most POLL_PERIOD_MS.
		isConnected = readFromMaster(socketFd, pReader);
		result = Framing_getMessage(pReader, &header, &pPayload);
	}
	// Figure out what happened
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

// This checks that the reply really is HELLO_ACK with the correct payload
// size.
// If the status is OK, we store the master's heartbeat_ms.
// If the master sends a heartbeat_ms of 0, we use the default instead,
// since 0 would mean sending a heartbeat on every POLL_PERIOD_MS loop.
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
		heartbeatPeriodMs = (ack.heartbeat_ms > 0) ? ack.heartbeat_ms : PROTO_HEARTBEAT_MS;
		printf("Connected to master as node %u (heartbeat every %lld ms).\n",
		       myNodeId, heartbeatPeriodMs);
	} else {
		// Otherwise print the reason for the rejection.
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
		// We don't assert(false) here. The status comes from the network (a
		// newer master could add one), so an unknown one isn't a bug in this
		// code, and asserting would let another machine crash this node.
		break;
	}
	return pDescription;
}

// Sends events and heartbeats until the connection drops or a shutdown is
// requested.
// pReader is the same reader sayHello() used, since the HELLO_ACK and the
// master's next message can arrive in the same recv(). A new reader would
// throw away whatever came after the HELLO_ACK.
static void runSession(int socketFd, Framing_reader_t *pReader)
{
	// Anything the DSP found while we were connecting is already a few
	// seconds old, which is too late to be useful for localization
	discardDspEvents();

	long long lastSendMs = Timing_getMonotonicMs();
	bool isConnected = true;
	while (isConnected && isThreadRunning()) {
		isConnected = readFromMaster(socketFd, pReader)
		              && handleMessagesFromMaster(pReader)
		              && sendDspEvents(socketFd, &lastSendMs);

		// Events update lastSendMs too, so a HEARTBEAT only goes out when the
		// node has been quiet for a heartbeat period (any message shows the
		// master that the node is alive)
		bool isHeartbeatDue = (Timing_getMonotonicMs() - lastSendMs >= heartbeatPeriodMs);
		if (isConnected && isHeartbeatDue) {
			isConnected = sendHeartbeat(socketFd);
			lastSendMs = Timing_getMonotonicMs();
		}
	}

	// If we are still connected, the only way out of the loop was a shutdown.
	// Saying BYE lets the master drop us now instead of waiting for a timeout.
	if (isConnected) {
		sendBye(socketFd, PROTO_BYE_SHUTDOWN);
	}
	printf("Disconnected from master.\n");
}

// Waits up to POLL_PERIOD_MS for bytes from the master, and reads them if any
// arrive. Returns false if the connection is gone.
static bool readFromMaster(int socketFd, Framing_reader_t *pReader)
{
	struct pollfd pollEntry = {.fd = socketFd, .events = POLLIN};
	int numReady = poll(&pollEntry, 1, POLL_PERIOD_MS);

	bool isConnected = true;
	if (numReady < 0) {
		isConnected = (errno == EINTR);
	} else if (numReady > 0) {
		// We read even if poll() woke us for POLLHUP or POLLERR instead of
		// data, since recv() then tells us what happened. 0 means the master
		// closed the connection, and -1 sets errno to the reason. This is
		// where a dead master shows up (ETIMEDOUT, once TCP_USER_TIMEOUT runs
		// out).
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

// Handles every complete message in the reader, not just the first one.
// poll() only wakes us for new bytes, so a second message left in the reader
// would sit there until the master sent something else (which it may never
// do). Returns false if the connection should be closed.
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
			// Unknown types are skipped instead of closing the connection.
			// 0x40 to 0x4F are saved for master to node commands, so a
			// newer master can add them without breaking older nodes.
			fprintf(stderr, "Ignoring message type 0x%02x from master.\n", header.msg_type);
			break;
		}
		result = Framing_getMessage(pReader, &header, &pPayload);
	}

	// A framing error means we can't trust anything else on this stream
	if (isConnected && result != FRAMING_NEED_MORE_BYTES) {
		isConnected = reportFramingError(result);
	}
	return isConnected;
}

// Every BYE closes the connection (the caller handles that). The reason only
// decides whether we reconnect afterwards.
static void handleBye(const uint8_t *pPayload, size_t payloadLen)
{
	struct proto_bye bye;
	if (proto_unpack_bye(pPayload, payloadLen, &bye) != PROTO_OK) {
		fprintf(stderr, "Master said BYE.\n");
	} else if (bye.reason == PROTO_BYE_REPLACED) {
		// The master replaces an old connection when a node with the same ID
		// says HELLO, to clean up after a node that rebooted. A node that
		// really rebooted is gone and never sees this BYE, so getting it means
		// another running node has our ID. If we reconnected, we would knock
		// that node off in turn, and the two would swap places every second.
		fprintf(stderr, "Another node connected as node %u, so this one is shutting down. "
		                "Check that every node has its own ID.\n", myNodeId);
		Shutdown_request();
	} else {
		fprintf(stderr, "Master said BYE (reason %u).\n", bye.reason);
	}
}

// Sends every event the DSP has ready. Returns false if the connection failed.
static bool sendDspEvents(int socketFd, long long *pLastSendMs)
{
	struct proto_event event;
	bool isConnected = true;
	while (isConnected && DspLink_takeEvent(&event)) {
		// Stamped right before sending, so the master can measure the
		// network latency (its clock minus this, once chrony has synced us)
		event.t_send_ns = Timing_getRealtimeNs();
		uint8_t message[PROTO_HEADER_LEN + PROTO_EVENT_LEN];
		size_t payloadLen = proto_pack_event(message + PROTO_HEADER_LEN, &event);
		isConnected = sendMessage(socketFd, message, PROTO_MSG_EVENT, payloadLen);
		*pLastSendMs = Timing_getMonotonicMs();
	}
	return isConnected;
}

// Tells the master this node is alive, how well its clock is synced, and how
// the DSP is doing.
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
// how well we are synced without having to talk to chrony.
static void fillClockStatus(struct proto_heartbeat *pHeartbeat)
{
	// modes = 0 makes adjtimex() read only, so it doesn't change the clock
	// and doesn't need root
	struct timex clockInfo = {0};
	int clockState = adjtimex(&clockInfo);
	pHeartbeat->clock_synced = (clockState >= 0 && clockState != TIME_ERROR);
	// esterror is the kernel's estimate of how far off the clock is (in µs)
	pHeartbeat->clock_err_us = (uint32_t)clockInfo.esterror;
}

static bool sendBye(int socketFd, uint8_t reason)
{
	uint8_t message[PROTO_HEADER_LEN + PROTO_BYE_LEN];
	struct proto_bye bye = {.reason = reason};
	size_t payloadLen = proto_pack_bye(message + PROTO_HEADER_LEN, &bye);
	return sendMessage(socketFd, message, PROTO_MSG_BYE, payloadLen);
}

// Sends a message whose payload is already packed at
// pMessage + PROTO_HEADER_LEN. Every message we send goes through here, so
// this is the only place that fills in our node ID and seq number.
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

// Every framing error means the stream can't be trusted, so this always
// returns false (the connection should close). Returning a value lets the
// caller write isConnected = reportFramingError(result).
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
		// Only called with an error result, so anything else is our bug
		assert(false);
	}
	return false;
}

// Sleeps for PROTO_RECONNECT_MS in short steps, so a shutdown isn't held up
// by a whole second.
static void waitToReconnect(void)
{
	long long endMs = Timing_getMonotonicMs() + PROTO_RECONNECT_MS;
	while (isThreadRunning() && Timing_getMonotonicMs() < endMs) {
		// We keep emptying the DSP's queue while disconnected. Otherwise it
		// fills up, and the DSP counts drops that aren't really its fault.
		discardDspEvents();
		Timing_sleepForMs(POLL_PERIOD_MS);
	}
}

static void discardDspEvents(void)
{
	struct proto_event event;
	while (DspLink_takeEvent(&event)) {
		// Thrown away, since there's no master to send it to
	}
}
