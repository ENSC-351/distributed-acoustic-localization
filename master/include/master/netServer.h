// netServer.h
// Network server module: in a background thread, accepts TCP connections
// from the nodes, answers their HELLOs, and puts the EVENTs they send into
// the event buffer
// HEARTBEATs are checked for clock and DSP problems, which are logged.
//
// Drop stale nodes 
// If a node connects with the ID of a node that's already connected


#ifndef NET_SERVER_H
#define NET_SERVER_H

#include <stdint.h>

// Start listening on port and start the server thread.
// Exits the program if the port can't be opened.
void NetServer_init(uint16_t port);

// Stop the thread, saying BYE to every connected node first
void NetServer_cleanup(void);

#endif
