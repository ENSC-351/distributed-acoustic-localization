#ifndef NET_CLIENT_H
#define NET_CLIENT_H

#include <stdint.h>

// Client networking module:
// The client attempts to reconnect every second and events are dropped while it
// is disconnected.
// If the client is initialized with a duplicate Id, it may call
// Shutdown_request() on itself, ending the whole program.

// Initialize networking.
// Only one net client should run per node.
// nodeId should be unique to the node in the network, MasterIp is the IP addr
// of the master node, port should be agreeed upon.
void NetClient_init(uint16_t nodeId, const char *pMasterIp, uint16_t port);

// Shuts down the networking thread.
// If the node is connected, it says BYE to the master first so the master
// doesn't have to wait for a timeout.
void NetClient_cleanup(void);

#endif
