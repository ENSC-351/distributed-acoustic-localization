// netServer.c
// Network server module: one thread uses poll() to watch the listening
// socket and every node's socket at once, so no node can hold up another.
//
//  - a node reconnecting with an ID that's already connected replaces the
//    old connection (which gets BYE with PROTO_BYE_REPLACED)
//  - HEARTBEAT problems (clock not synced, DSP not alive, ring drops) are
//    logged when they change
//  - a HELLO with the wrong protocol version gets PROTO_HELLO_VERSION
//  - event_id gaps are NOT checked: the node counts windows it chose not to
//    send, so a gap is normal (master/LOCALIZER_HANDOFF.md)

#include "master/netServer.h"
#include "master/eventBuffer.h"
#include "dashboard.h"
#include "common/framing.h"
#include "common/protocol.h"
#include "common/timing.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

//most connections open at once, including ones that haven't said HELLO yet
#define MAX_CLIENTS 8

// How long poll() waits before the thread checks for timeouts and shutdown
#define POLL_PERIOD_MS 100

#define LISTEN_BACKLOG 8

//connections should be in one of these states
typedef enum {
	CLIENT_FREE, // slot is unused
	CLIENT_AWAITING_HELLO, // connected, but hasn't said who it is
	CLIENT_ACTIVE, // accepted with HELLO_ACK
} ClientState_t;

// Everything the server remembers about one connection
typedef struct {
	ClientState_t state; 			//defined above
	int socketFd;
	char address[INET_ADDRSTRLEN];	// dotted IPv4, for log messages
	uint16_t nodeId;				//only valid once CLIENT_ACTIVE
	uint32_t nextSeq;               //for messages we send this client
	long long lastRecvMs;           // CLOCK_MONOTONIC of the last bytes received
	Framing_reader_t reader;        // one per connection: each has its own partial message

	// Health from the last HEARTBEAT, so problems are logged once when they
	// change instead of every second
	bool clockSynced;
	bool dspAlive;
	uint32_t ringDrops;             // the node's running total
} Client_t;

// Thread start/stop state (from netClient.c)
static pthread_t netServerThread;
static bool isRunning = false;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// Set before the thread starts, then only used by the thread
static int listenFd = -1;
static Client_t clients[MAX_CLIENTS]; //this array of structs contains all the clients that are connected to the server

static bool isThreadRunning(void);
static int openListenSocket(uint16_t port);

static void acceptClient(void);
static Client_t *findFreeClient(void);

static void readFromClient(Client_t *pClient);
static void handleMessages(Client_t *pClient);
static bool handleHello(Client_t *pClient, const struct proto_header *pHeader,
                        const uint8_t *pPayload);
static bool handleSessionMessage(Client_t *pClient, const struct proto_header *pHeader,
                                 const uint8_t *pPayload);
static void handleEvent(Client_t *pClient, const struct proto_event *pEvent);
static void handleHeartbeat(Client_t *pClient, const struct proto_heartbeat *pHeartbeat);

static void dropSilentClients(void);
static void closeAllClients(void);
static void replaceClientConnection(Client_t *pNewClient);
static void closeClient(Client_t *pClient);

static bool sendHelloAck(Client_t *pClient, uint8_t status);
static bool sendBye(Client_t *pClient, uint8_t reason);
static bool sendMessage(Client_t *pClient, uint8_t *pMessage, uint8_t msgType, size_t payloadLen);

static void *netServerThreadFn(void *pArg)
{
	(void)pArg;
	while (isThreadRunning()) {
		//entry 0 is the listening socket and the rest are clients
		struct pollfd pollEntries[1 + MAX_CLIENTS];
		Client_t *pClientOfEntry[1 + MAX_CLIENTS];
		nfds_t numEntries = 0;
		pollEntries[numEntries] = (struct pollfd){.fd = listenFd, .events = POLLIN};
		pClientOfEntry[numEntries] = NULL;
		numEntries++;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].state != CLIENT_FREE) {
				pollEntries[numEntries] = (struct pollfd){.fd = clients[i].socketFd, .events = POLLIN};
				pClientOfEntry[numEntries] = &clients[i];
				numEntries++;
			}
		}

		int numReady = poll(pollEntries, numEntries, POLL_PERIOD_MS);
		if (numReady < 0 && errno != EINTR) {
			perror("NetServer poll");
		}
		if (numReady > 0) {
			for (nfds_t i = 1; i < numEntries; i++) {
				// Also covers POLLHUP and POLLERR: recv() then reports what went wrong
				if (pollEntries[i].revents != 0 && pClientOfEntry[i]->state != CLIENT_FREE) {
					readFromClient(pClientOfEntry[i]);
				}
			}
			// Accept last, so a new client can't take a slot that's in this loop
			if (pollEntries[0].revents & POLLIN) {
				acceptClient();
			}
		}
		dropSilentClients();
	}

	closeAllClients();
	return NULL;
}

void NetServer_init(uint16_t port)
{
	// (from netClient.c)
	pthread_mutex_lock(&lock);
	if (isRunning) { //check that the thread isn't already running
		pthread_mutex_unlock(&lock);
		fprintf(stderr, "The netServer thread is already running.\n");
		return;
	}
	isRunning = true;
	pthread_mutex_unlock(&lock);

	for (int i = 0; i < MAX_CLIENTS; i++) { //initialize all clients in the clients arrayto free state
		clients[i] = (Client_t){.state = CLIENT_FREE, .socketFd = -1};
	}
	listenFd = openListenSocket(port); //open the listening socket, or -1 if it failed
	if (listenFd < 0) {
		exit(EXIT_FAILURE);
	}
	printf("Listening for nodes on port %u.\n", port);

	int result = pthread_create(&netServerThread, NULL, netServerThreadFn, NULL);//create the thread that runs the server
	if (result != 0) {
		fprintf(stderr, "Failed to create netServer thread!\n");
		close(listenFd);
		listenFd = -1;
		pthread_mutex_lock(&lock);
		isRunning = false; //signal that the thread inst running (should do smt about this)
		pthread_mutex_unlock(&lock);
	}
}

void NetServer_cleanup(void) // (from netClient.c)
{
	pthread_mutex_lock(&lock);
	if (!isRunning) {
		pthread_mutex_unlock(&lock);
		return; // Thread wasn't running
	}
	isRunning = false;
	pthread_mutex_unlock(&lock);

	// The thread never blocks for longer than POLL_PERIOD_MS, so it notices
	// quickly and closes every connection on its way out
	pthread_join(netServerThread, NULL);
	close(listenFd);
	listenFd = -1;
	printf("NetServer thread cleaned up successfully.\n");
}

//same as netClient.c
static bool isThreadRunning(void)
{
	pthread_mutex_lock(&lock);
	bool shouldRun = isRunning;
	pthread_mutex_unlock(&lock);
	return shouldRun;
}

// Returns the listening socket, or -1 (after printing why) if it failed
static int openListenSocket(uint16_t port)
{
	int socketFd = socket(AF_INET, SOCK_STREAM, 0); //create a TCP socket
	if (socketFd < 0) { //socketFd returns -1 if it failed to create the socket
		perror("NetServer socket not created");
		return -1;
	}

	// Without SO_REUSEADDR, restarting the master fails with "Address already in
	// use" for about a minute, while the old connections sit in TIME_WAIT
	int isReuseAddr = 1;
	setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &isReuseAddr, sizeof(isReuseAddr));

	struct sockaddr_in address = { //bind to all interfaces, not just localhost
		.sin_family = AF_INET, 					//IPv4
		.sin_port = htons(port),				//host to network short byte order
		.sin_addr.s_addr = htonl(INADDR_ANY),	//host to network long byte order
	};

	// bind the socket to the address and port, then listen for connections
	//if either bind or listen fails, isOk will be false
	bool isOk = (bind(socketFd, (struct sockaddr *)&address, sizeof(address)) == 0) && (listen(socketFd, LISTEN_BACKLOG) == 0); 
	if (!isOk) {
		fprintf(stderr, "Can't listen on port %u: %s\n", port, strerror(errno));
		close(socketFd);
		socketFd = -1;
	}
	return socketFd;
}

static void acceptClient(void)
{
	struct sockaddr_in address;
	socklen_t addressLen = sizeof(address); 
	int socketFd = accept(listenFd, (struct sockaddr *)&address, &addressLen); //take the connectiion out of the listen() queue
	if (socketFd < 0) {
		perror("NetServer accept");
		return;
	}

	Client_t *pClient = findFreeClient();
	if (pClient == NULL) { //find a free slot in the clients array to store the new client, if there isn't one, close the connection
		fprintf(stderr, "Refusing a connection: already have %d.\n", MAX_CLIENTS);
		close(socketFd);
		return;
	}

	*pClient = (Client_t){ //initialize the new client struct
		.state = CLIENT_AWAITING_HELLO, //the client is connected, but hasn't said who it is yet
		.socketFd = socketFd, //the socket for this client
		.lastRecvMs = Timing_getMonotonicMs(), //set the last received time to now
		.clockSynced = true, // assume healthy, so a bad first HEARTBEAT gets logged
		.dspAlive = true,
		//.nextSeq = 0, //the next sequence number for messages we send to this. i dont think we need this may be useful for debugging
	};
	inet_ntop(AF_INET, &address.sin_addr, pClient->address, sizeof(pClient->address));//convert the IP address to a string and store it in the client struct
	Framing_initReader(&pClient->reader);
	printf("Connection from %s, waiting for HELLO.\n", pClient->address);
}

static Client_t *findFreeClient(void)
{
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].state == CLIENT_FREE) {
			return &clients[i];
		}
	}
	return NULL;
}

// (like readFromMaster() in netClient.c)
static void readFromClient(Client_t *pClient)
{
	ssize_t numBytesRead = Framing_readFromSocket(&pClient->reader, pClient->socketFd);
	if (numBytesRead == 0) {
		printf("%s closed the connection.\n", pClient->address);
		closeClient(pClient);
	} else if (numBytesRead < 0) {
		if (errno != EINTR) {
			fprintf(stderr, "%s: recv failed: %s\n", pClient->address, strerror(errno));
			closeClient(pClient);
		}
	} else {
		pClient->lastRecvMs = Timing_getMonotonicMs();
		handleMessages(pClient);
	}
}

// Handle every complete message the client has sent, closing the
// connection if one of them calls for it (like handleMessagesFromMaster())
static void handleMessages(Client_t *pClient)
{
	struct proto_header header;
	const uint8_t *pPayload = NULL;
	bool isConnected = true;
	Framing_result_t result = Framing_getMessage(&pClient->reader, &header, &pPayload); //get the next complete message from the client, if there is one
	while (isConnected && result == FRAMING_MESSAGE_READY) {
		if (pClient->state == CLIENT_AWAITING_HELLO) { //if the client hasn't said who it is yet handle the message as a HELLO
			isConnected = handleHello(pClient, &header, pPayload);
		} else { //otherwise handle the message as a session message
			isConnected = handleSessionMessage(pClient, &header, pPayload);
		}
		if (isConnected) {
			result = Framing_getMessage(&pClient->reader, &header, &pPayload);
		}
	}

	if (isConnected && result != FRAMING_NEED_MORE_BYTES) {
		// A HELLO from a node with a different protocol version: the header was
		// still filled in, so tell the node why before closing, instead of
		// leaving it to guess
		if (result == FRAMING_BAD_VERSION && pClient->state == CLIENT_AWAITING_HELLO &&
		    header.msg_type == PROTO_MSG_HELLO) {
			fprintf(stderr, "%s: HELLO with protocol version %u (we use %u), rejecting.\n",
			        pClient->address, header.version, PROTO_VERSION);
			sendHelloAck(pClient, PROTO_HELLO_VERSION);
		} else {
			//bad magic, version, or length: the stream can't be trusted
			fprintf(stderr, "%s: framing error %d, closing.\n", pClient->address, result);
		}
		isConnected = false;
	}
	if (!isConnected) {
		closeClient(pClient);
	}
}

// Returns false if the connection should be closed
static bool handleHello(Client_t *pClient, const struct proto_header *pHeader, const uint8_t *pPayload)
{
	struct proto_hello hello;
	if (pHeader->msg_type != PROTO_MSG_HELLO || proto_unpack_hello(pPayload, pHeader->payload_len, &hello) != PROTO_OK) { //
		//the first message from a client should be a HELLO, if it isn't, close the connection
		fprintf(stderr, "%s: first message wasn't a valid HELLO.\n", pClient->address);
		return false;
	}

	pClient->state = CLIENT_ACTIVE;
	pClient->nodeId = pHeader->node_id;
	replaceClientConnection(pClient); // must come after nodeId is set
	bool isConnected = sendHelloAck(pClient, PROTO_HELLO_OK);
	if (isConnected) {
		printf("Node %u connected from %s (%u mics).\n",
		       pClient->nodeId, pClient->address, hello.mic_count);
		Dashboard_setNodeConnected(pClient->nodeId, true); // web GUI: link up
	}
	return isConnected;
}

// Handle a message from a node that has already said HELLO. Returns false
// if the connection should be closed.
static bool handleSessionMessage(Client_t *pClient, const struct proto_header *pHeader, const uint8_t *pPayload)
{
	bool isConnected = true;
	struct proto_event event;
	struct proto_heartbeat heartbeat;
	switch (pHeader->msg_type) {
	case PROTO_MSG_EVENT:
		if (proto_unpack_event(pPayload, pHeader->payload_len, &event) == PROTO_OK) {
			handleEvent(pClient, &event);
		}
		break;
	case PROTO_MSG_HEARTBEAT:
		//receiving it already updated lastRecvMs, which is its main job.
		if (proto_unpack_heartbeat(pPayload, pHeader->payload_len, &heartbeat) == PROTO_OK) {
			Dashboard_touchNode(pClient->nodeId); // web GUI: "Last message"
			handleHeartbeat(pClient, &heartbeat);
		}
		break;
	case PROTO_MSG_BYE:
		printf("Node %u said BYE.\n", pClient->nodeId);
		isConnected = false;
		break;
	default:
		//skip types this build doesn't know, so nodes can add messages without breaking an older master
		fprintf(stderr, "Node %u: ignoring message type 0x%02x.\n",
		        pClient->nodeId, pHeader->msg_type);
		break;
	}
	return isConnected;
}

static void handleEvent(Client_t *pClient, const struct proto_event *pEvent)
{
	//printed here for now, since the localizer can't take events yet
	printf("Node %u event %u: theta %.1f\n", pClient->nodeId, pEvent->event_id, pEvent->theta);

	EventBuffer_entry_t entry = { // create an entry to add to the event buffer, with the node ID, the time it was received, and the event itself
		.nodeId = pClient->nodeId,
		.recvTimeNs = Timing_getRealtimeNs(),
		.event = *pEvent,
	};
	EventBuffer_add(&entry);
}

// Log a node's health problems only when they change, since a HEARTBEAT
// arrives about every second
static void handleHeartbeat(Client_t *pClient, const struct proto_heartbeat *pHeartbeat)
{
	bool isClockSynced = (pHeartbeat->clock_synced != 0);
	if (isClockSynced != pClient->clockSynced) {
		if (isClockSynced) {
			printf("Node %u: clock synced again.\n", pClient->nodeId);
		} else {
			fprintf(stderr, "Node %u: clock NOT synced (est. error %u us), its timestamps are unreliable.\n",
			        pClient->nodeId, pHeartbeat->clock_err_us);
		}
		pClient->clockSynced = isClockSynced;
	}

	bool isDspAlive = (pHeartbeat->dsp_alive != 0);
	if (isDspAlive != pClient->dspAlive) {
		if (isDspAlive) {
			printf("Node %u: DSP is running again.\n", pClient->nodeId);
		} else {
			fprintf(stderr, "Node %u: DSP is not producing audio.\n", pClient->nodeId);
		}
		pClient->dspAlive = isDspAlive;
	}

	// ring_drops is a running total, so only the increase is new
	if (pHeartbeat->ring_drops > pClient->ringDrops) {
		fprintf(stderr, "Node %u: DSP dropped %u events (total %u).\n",
		        pClient->nodeId, pHeartbeat->ring_drops - pClient->ringDrops,
		        pHeartbeat->ring_drops);
	}
	pClient->ringDrops = pHeartbeat->ring_drops;
}

static void dropSilentClients(void)
{
	long long nowMs = Timing_getMonotonicMs();
	//loop over all clients and check if they have been silent for too long, if so drop them
	for (int i = 0; i < MAX_CLIENTS; i++) {
		Client_t *pClient = &clients[i];
		if (pClient->state != CLIENT_FREE && nowMs - pClient->lastRecvMs > PROTO_TIMEOUT_MS) {
			fprintf(stderr, "%s sent nothing for %d ms, dropping it.\n",
			        pClient->address, PROTO_TIMEOUT_MS);
			closeClient(pClient);
		}
	}
}

// Says BYE to every active node first, so the nodes know the master is
// shutting down instead of waiting for a timeout
static void closeAllClients(void)
{
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].state == CLIENT_ACTIVE) {
			sendBye(&clients[i], PROTO_BYE_SHUTDOWN);
		}
		if (clients[i].state != CLIENT_FREE) {
			closeClient(&clients[i]);
		}
	}
}

// If another active slot already has pNewClient's node ID, it's a stale
// connection from the same node: tell it BYE (replaced) and close it.
// Call only after pNewClient->nodeId has been set.
static void replaceClientConnection(Client_t *pNewClient)
{
	for (int i = 0; i < MAX_CLIENTS; i++) {
		Client_t *pOld = &clients[i];

		// active, same node ID, and not the new connection itself
		if (pOld->state == CLIENT_ACTIVE &&
		    pOld->nodeId == pNewClient->nodeId &&
		    pOld != pNewClient) {
			printf("Node %u reconnected from %s, replacing old connection.\n",
			       pNewClient->nodeId, pNewClient->address);

			// BYE first: closeClient() closes the socket, so nothing could be sent after
			sendBye(pOld, PROTO_BYE_REPLACED);
			closeClient(pOld);
		}
	}
}

static void closeClient(Client_t *pClient)
{
	if (pClient->state == CLIENT_ACTIVE) {
		printf("Node %u disconnected.\n", pClient->nodeId);
		Dashboard_setNodeConnected(pClient->nodeId, false); // web GUI: link down
	}
	//close socket, mark as free and update state
	close(pClient->socketFd);
	pClient->socketFd = -1;
	pClient->state = CLIENT_FREE;
}

static bool sendHelloAck(Client_t *pClient, uint8_t status)
{
	uint8_t message[PROTO_HEADER_LEN + PROTO_HELLO_ACK_LEN];
	struct proto_hello_ack ack = {.status = status, .heartbeat_ms = PROTO_HEARTBEAT_MS};
	size_t payloadLen = proto_pack_hello_ack(message + PROTO_HEADER_LEN, &ack);
	return sendMessage(pClient, message, PROTO_MSG_HELLO_ACK, payloadLen); //returns false if the send failed, which will cause the connection to close
}

static bool sendBye(Client_t *pClient, uint8_t reason)
{
	uint8_t message[PROTO_HEADER_LEN + PROTO_BYE_LEN];
	struct proto_bye bye = {.reason = reason};
	size_t payloadLen = proto_pack_bye(message + PROTO_HEADER_LEN, &bye);
	return sendMessage(pClient, message, PROTO_MSG_BYE, payloadLen);
}

// Send a message whose payload is already packed at pMessage + PROTO_HEADER_LEN
// (from netClient.c, but the seq counter is per client and node_id is the master's)
static bool sendMessage(Client_t *pClient, uint8_t *pMessage, uint8_t msgType, size_t payloadLen)
{
	struct proto_header header = proto_make_header(msgType, (uint16_t)payloadLen, PROTO_MASTER_ID, pClient->nextSeq);
	pClient->nextSeq++;
	bool isSent = Framing_sendMessage(pClient->socketFd, pMessage, &header); //returns false if the send failed, the connection will be closed in handleMessages()
	if (!isSent) {
		fprintf(stderr, "%s: send failed: %s\n", pClient->address, strerror(errno));
	}
	return isSent;
}
