// shutdown.h
// The shutdown module: it lets any thread ask the program to end, and lets
// main block until that happens. SIGINT (Ctrl+C) and SIGTERM (systemctl stop)
// also count as a request.

#ifndef SHUTDOWN_H
#define SHUTDOWN_H

// Call this first, before any other thread is created, so that every thread
// inherits SIGINT and SIGTERM as blocked.
void Shutdown_init(void);

// Ask the program to shut down. This is safe to call from any thread, and
// safe to call more than once.
void Shutdown_request(void);

// Block the calling thread until Shutdown_request() is called.
void Shutdown_waitForRequest(void);

// Stop the signal thread. Call this last.
void Shutdown_cleanup(void);

#endif
