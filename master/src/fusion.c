#include "fusion.h"
#include "dashboard.h"
#include "layout.h"
#include "locate.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MS_PER_S 1000
#define NS_PER_MS 1000000
#define POLL_PERIOD_MS 10
// Lines need some weight even if a node reports 0 confidence
#define MIN_WEIGHT 0.05f

/**
 * @brief Reports collected so far for the current sound
 */
typedef struct {
    bool isOpen;
    int64_t openedMs;
    int count;
    DashboardBearing bearings[LAYOUT_NUM_NODES];
} Group;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Group group;
static pthread_t pollThread;
static bool isInitialized = false;
static atomic_bool isRunning = false;

static int64_t nowMs(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * MS_PER_S + now.tv_nsec / NS_PER_MS;
}

/**
 * @brief Turns the open group into a fix and empties it. Caller holds the lock.
 */
static void closeGroup(void)
{
    if (!group.isOpen) {
        return;
    }
    DashboardFix fix;
    memset(&fix, 0, sizeof(fix));
    fix.numBearings = group.count;

    LocateLine lines[LAYOUT_NUM_NODES];
    for (int i = 0; i < group.count; i++) {
        fix.bearings[i] = group.bearings[i];
        const LayoutNode *node = Layout_getById(group.bearings[i].nodeId);
        lines[i].x = node->x;
        lines[i].y = node->y;
        lines[i].bearingDeg = group.bearings[i].bearingDeg;
        lines[i].weight = (group.bearings[i].confidence > MIN_WEIGHT)
                              ? group.bearings[i].confidence : MIN_WEIGHT;
    }

    LocateResult result;
    if (Locate_intersect(lines, group.count, &result)) {
        fix.hasPosition = true;
        fix.x = result.x;
        fix.y = result.y;
        fix.rmsM = result.rmsM;
        fix.isInFront = result.isInFront;
    }
    Dashboard_addFix(&fix);
    memset(&group, 0, sizeof(group));
}

void Fusion_addEvent(uint16_t nodeId, float thetaDeg, float confidence, uint16_t flags)
{
    Dashboard_recordEvent(nodeId, thetaDeg, confidence, flags);
    const LayoutNode *node = Layout_getById(nodeId);
    if (node == NULL) {
        return;
    }

    pthread_mutex_lock(&lock);
    // A second report from the same node means a new sound
    for (int i = 0; i < group.count; i++) {
        if (group.bearings[i].nodeId == nodeId) {
            closeGroup();
            break;
        }
    }
    if (!group.isOpen) {
        group.isOpen = true;
        group.openedMs = nowMs();
    }
    DashboardBearing *bearing = &group.bearings[group.count++];
    bearing->nodeId = nodeId;
    bearing->thetaDeg = thetaDeg;
    bearing->bearingDeg = Layout_thetaToBearingDeg(node, thetaDeg);
    bearing->confidence = confidence;

    if (group.count == LAYOUT_NUM_NODES) {
        closeGroup();
    }
    pthread_mutex_unlock(&lock);
}

/**
 * @brief Closes the group once its window has run out
 */
static void *pollThreadFn(void *arg)
{
    (void)arg;
    struct timespec period = {.tv_sec = 0, .tv_nsec = POLL_PERIOD_MS * NS_PER_MS};
    while (atomic_load(&isRunning)) {
        nanosleep(&period, NULL);
        pthread_mutex_lock(&lock);
        if (group.isOpen && nowMs() - group.openedMs >= FUSION_WINDOW_MS) {
            closeGroup();
        }
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

void Fusion_init(void)
{
    if (isInitialized) {
        return;
    }
    pthread_mutex_lock(&lock);
    memset(&group, 0, sizeof(group));
    pthread_mutex_unlock(&lock);

    atomic_store(&isRunning, true);
    int err = pthread_create(&pollThread, NULL, pollThreadFn, NULL);
    if (err != 0) {
        fprintf(stderr, "Unable to create fusion thread: %s\n", strerror(err));
        exit(EXIT_FAILURE);
    }
    isInitialized = true;
}

void Fusion_cleanup(void)
{
    if (!isInitialized) {
        return;
    }
    atomic_store(&isRunning, false);
    pthread_join(pollThread, NULL);
    pthread_mutex_lock(&lock);
    closeGroup();
    pthread_mutex_unlock(&lock);
    isInitialized = false;
}
