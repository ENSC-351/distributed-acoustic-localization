// shutdown.c handles shutdown requests by threads and signals (ideally gracefully)

#include "common/shutdown.h"
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static bool isShutdownRequested = false;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t requestedCondition = PTHREAD_COND_INITIALIZER;

static pthread_t signalThread;
static sigset_t shutdownSignals;

// The job of this thread is to wait for, and eventuall turn a CTRL+C into a
// Shutdown_request() call.
// Waiting in a normal thread, instead of catching the signal with a signal
// handler, means Shutdown_request() is free to lock a mutex. A handler runs
// on top of whichever thread the signal interrupted, and if that thread
// already holds the lock, locking it again deadlocks.
static void *signalThreadFn(void *pArg)
{
	(void)pArg;
	int signalNumber = 0;
	// Sleep until one of the signals in the set is pending
	sigwait(&shutdownSignals, &signalNumber);
	Shutdown_request();
	return NULL;
}

void Shutdown_init(void)
{
	// Build the set of potential shutdown signals
	sigemptyset(&shutdownSignals);
	sigaddset(&shutdownSignals, SIGINT);
	sigaddset(&shutdownSignals, SIGTERM);

	// Blocked signals are not thrown away, they are held by the kernal as
	// "pending" until someone (a thread) is ready to deal with it.
	// pthread_sigmask() only changes the calling thread, but threads created
	// afterwards copy its mask, so every thread started after this has the
	// signals blocked too. Only sigwait() in the signal thread will take them.
	int result = pthread_sigmask(SIG_BLOCK, &shutdownSignals, NULL);
	if (result != 0) {
		fprintf(stderr, "Failed to block shutdown signals!\n");
		exit(EXIT_FAILURE);
	}

	result = pthread_create(&signalThread, NULL, signalThreadFn, NULL);
	if (result != 0) {
		fprintf(stderr, "Failed to create signal thread!\n");
		exit(EXIT_FAILURE);
	}
}

void Shutdown_request(void)
{
	pthread_mutex_lock(&lock);
	isShutdownRequested = true;
	pthread_cond_broadcast(&requestedCondition);
	pthread_mutex_unlock(&lock);
}

void Shutdown_waitForRequest(void)
{
	pthread_mutex_lock(&lock);
	// Loop because pthread_cond_wait() can return without a broadcast
	// (a spurious wakeup)
	while (!isShutdownRequested) {
		pthread_cond_wait(&requestedCondition, &lock);
	}
	pthread_mutex_unlock(&lock);
}

void Shutdown_cleanup(void)
{
	// pthread_kill() is used to prevent a hang in the following scenario:
	// If a shutdown doesn't come from a signal, the thread will still be stuck in
	// sigwait(), therefore pthread_join() would wait forever.
	// The shutdown signal only goes to the signalThread. The signalThread has the
	// shutdown signal blocked, so it either wakes up sigwait() or it is discarded
	// and the default action is never reached.
	pthread_kill(signalThread, SIGTERM);
	pthread_join(signalThread, NULL);
}
