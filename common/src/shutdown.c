// shutdown.c handles shutdown requests by threads and signals (gracefully)

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

static void *signalThreadFn(void *pArg)
{
	(void)pArg;
	int signalNumber = 0;
	// Waiting for the signal in a normal thread, instead of catching it with
	// a signal handler, means Shutdown_request() is free to lock a mutex.
	// A handler can interrupt a thread that already holds that mutex.
	sigwait(&shutdownSignals, &signalNumber);
	Shutdown_request();
	return NULL;
}

void Shutdown_init(void)
{
	sigemptyset(&shutdownSignals);
	sigaddset(&shutdownSignals, SIGINT);
	sigaddset(&shutdownSignals, SIGTERM);

	// New threads copy the signal mask of the thread that creates them, so
	// blocking the signals here blocks them in every thread started later.
	// Only sigwait() in the signal thread will see them.
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
	// Wake every thread waiting in Shutdown_waitForRequest()
	pthread_cond_broadcast(&requestedCondition);
	pthread_mutex_unlock(&lock);
}

void Shutdown_waitForRequest(void)
{
	pthread_mutex_lock(&lock);
	// Loop because pthread_cond_wait() can return without a broadcast
	while (!isShutdownRequested) {
		pthread_cond_wait(&requestedCondition, &lock);
	}
	pthread_mutex_unlock(&lock);
}

void Shutdown_cleanup(void)
{
	// If the shutdown wasn't caused by a signal, the signal thread is still
	// stuck in sigwait(), so hand it one. The signal is blocked, so this
	// can't kill the process.
	pthread_kill(signalThread, SIGTERM);
	pthread_join(signalThread, NULL);
}
