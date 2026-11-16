// netClient.h
// Network client module: in a background thread, keeps a TCP connection to
// the master and sends it this node's EVENTs and HEARTBEATs (see
// common/protocol.h for the messages).
//
// If the connection drops, it tries again every PROTO_RECONNECT_MS. Events
// the DSP finds while disconnected are thrown away, since a late event is
// useless for localization.
//
// The exception is BYE(REPLACED), which means another node has the same ID.
// Then it calls Shutdown_request() instead of reconnecting.

#ifndef NET_CLIENT_H
#define NET_CLIENT_H

#include <stdint.h>

// Start the thread that connects to the master at pMasterIp (dotted IPv4,
// like "192.168.10.1") and port, as node nodeId.
// Exits the program if pMasterIp isn't a valid address.
void NetClient_init(uint16_t nodeId, const char *pMasterIp, uint16_t port);

// Stop the thread, saying BYE to the master first if connected
void NetClient_cleanup(void);

#endif
