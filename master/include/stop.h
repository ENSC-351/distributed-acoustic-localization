/**
 * @file stop.h
 * @author Madden Wynans
 * @brief Lets any thread (or a signal handler) request that the master stop
 * @details Ported from Assignment 3.
 */
#ifndef STOP_H
#define STOP_H

/**
 * @brief Initializes the stop semaphore
 * @warning Must be called before any other stop function
 */
void Stop_init(void);

/**
 * @brief Cleans up the stop semaphore
 */
void Stop_cleanup(void);

/**
 * @brief Requests that the program stop
 * @details Async-signal-safe (only calls sem_post), so a SIGINT handler may call it.
 */
void Stop_request(void);

/**
 * @brief Blocks the calling thread until Stop_request() is called
 * @details Returns immediately if a stop was already requested
 */
void Stop_waitForRequest(void);

#endif
