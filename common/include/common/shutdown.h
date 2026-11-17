// shutdown.h
// Shutdown module: lets any thread ask the program to end, and lets main
// block until that happens. Ctrl+C (SIGINT) and SIGTERM also count as a request.

#ifndef SHUTDOWN_H
#define SHUTDOWN_H

// Blocks SIGINT and SIGTERM and starts the thread that waits for them.
// Call this first in main, before any other thread is created. A thread that
// already exists keeps the signals unblocked, and if Ctrl+C lands on it, the
// default action kills the whole process with no cleanup.
void Shutdown_init(void);

// Ask the program to shut down. Safe to call from any thread, and more than once.
void Shutdown_request(void);

// Block the calling thread until Shutdown_request() is called.
void Shutdown_waitForRequest(void);

// Stop the signal thread. Call this after Shutdown_waitForRequest() has
// returned, normally last. It doesn't hang if no signal ever arrived.
void Shutdown_cleanup(void);

#endif
