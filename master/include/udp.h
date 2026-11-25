/**
 * @file udp.h
 * @author Madden Wynans
 * @brief UDP server on port 12345 for the master's web GUI
 * @details Ported from Assignment 3. One command per packet, one reply each:
 *          "status": the dashboard as one line of JSON
 *          "sound":  demo mode only: make a pretend sound now ("ok")
 *          "help":   the list of commands
 *          "stop":   replies, then requests the master to stop
 *          Command words are case-insensitive. Unknown commands get "Unknown command.".
 *          Only the Node.js server on the same board is meant to talk to it.
 */
#ifndef UDP_H
#define UDP_H

#define UDP_PORT 12345

/**
 * @brief Opens the UDP socket and starts the server thread
 */
void UDP_init(void);

/**
 * @brief Stops the server thread and closes the socket
 * @details Safe to call whether the thread ended by itself ("stop") or not.
 */
void UDP_cleanup(void);

#endif
