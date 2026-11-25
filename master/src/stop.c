#include "stop.h"
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define SEMAPHORE_SHARED_BETWEEN_THREADS 0
#define SEMAPHORE_INITIAL_COUNT 0

static sem_t stopSemaphore;
static bool isInitialized = false;

void Stop_init(void)
{
    if (isInitialized) {
        return;
    }
    if (sem_init(&stopSemaphore, SEMAPHORE_SHARED_BETWEEN_THREADS, SEMAPHORE_INITIAL_COUNT) != 0) {
        perror("Unable to create stopping semaphore");
        exit(EXIT_FAILURE);
    }
    isInitialized = true;
}

void Stop_cleanup(void)
{
    if (!isInitialized) {
        return;
    }
    sem_destroy(&stopSemaphore);
    isInitialized = false;
}

void Stop_request(void)
{
    sem_post(&stopSemaphore);
}

void Stop_waitForRequest(void)
{
    sem_wait(&stopSemaphore);
}
