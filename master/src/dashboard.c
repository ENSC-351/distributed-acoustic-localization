#include "dashboard.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define NEVER_MS (-1)
#define MS_PER_S 1000
#define NS_PER_MS 1000000
#define EMPTY_JSON "{}"

/**
 * @brief Everything the page shows about one node
 */
typedef struct {
    bool isUsed;
    uint16_t id;
    float x;
    float y;
    float headingDeg;
    bool isConnected;
    int64_t lastSeenMs;     // NEVER_MS if no message yet
    uint32_t eventCount;
    float thetaDeg;
    float confidence;
    uint16_t flags;
    int64_t lastEventMs;    // NEVER_MS if no event yet
} NodeState;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static NodeState nodes[DASHBOARD_MAX_NODES];
static bool isDemoData = false;
static int64_t startMs = 0;

/**
 * @brief A fix plus the bookkeeping the dashboard adds
 */
typedef struct {
    DashboardFix fix;
    uint32_t id;
    int64_t timeMs;
} StoredFix;

// Ring of recent fixes; fixHead is where the next one goes
static StoredFix fixes[DASHBOARD_FIX_HISTORY];
static int fixCount = 0;
static int fixHead = 0;
static uint32_t nextFixId = 1;

/**
 * @brief Milliseconds on the monotonic clock (not affected by the board's wrong date)
 */
static int64_t nowMs(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * MS_PER_S + now.tv_nsec / NS_PER_MS;
}

/**
 * @brief Milliseconds since `thenMs`, or NEVER_MS if it never happened
 */
static int64_t ageMs(int64_t thenMs, int64_t now)
{
    return (thenMs == NEVER_MS) ? NEVER_MS : now - thenMs;
}

/**
 * @brief Finds a node's slot. Caller holds the lock.
 * @param create true to take a free slot if the node is new
 * @return the slot, or NULL if not found (or table full)
 */
static NodeState *findNode(uint16_t nodeId, bool create)
{
    if (nodeId == 0) {
        return NULL;
    }
    NodeState *freeSlot = NULL;
    for (int i = 0; i < DASHBOARD_MAX_NODES; i++) {
        if (nodes[i].isUsed && nodes[i].id == nodeId) {
            return &nodes[i];
        }
        if (!nodes[i].isUsed && freeSlot == NULL) {
            freeSlot = &nodes[i];
        }
    }
    if (!create || freeSlot == NULL) {
        return NULL;
    }
    memset(freeSlot, 0, sizeof(*freeSlot));
    freeSlot->isUsed = true;
    freeSlot->id = nodeId;
    freeSlot->lastSeenMs = NEVER_MS;
    freeSlot->lastEventMs = NEVER_MS;
    return freeSlot;
}

void Dashboard_init(bool isDemo)
{
    pthread_mutex_lock(&lock);
    memset(nodes, 0, sizeof(nodes));
    isDemoData = isDemo;
    startMs = nowMs();
    fixCount = 0;
    fixHead = 0;
    nextFixId = 1;
    pthread_mutex_unlock(&lock);
}

void Dashboard_cleanup(void)
{
    // Nothing allocated; kept so main() reads like the other modules
}

bool Dashboard_setNodePose(uint16_t nodeId, float x, float y, float headingDeg)
{
    pthread_mutex_lock(&lock);
    NodeState *node = findNode(nodeId, true);
    if (node != NULL) {
        node->x = x;
        node->y = y;
        node->headingDeg = headingDeg;
    }
    pthread_mutex_unlock(&lock);
    return node != NULL;
}

void Dashboard_setNodeConnected(uint16_t nodeId, bool isConnected)
{
    pthread_mutex_lock(&lock);
    NodeState *node = findNode(nodeId, true);
    if (node != NULL) {
        node->isConnected = isConnected;
        if (isConnected) {
            node->lastSeenMs = nowMs();
        }
    }
    pthread_mutex_unlock(&lock);
}

void Dashboard_touchNode(uint16_t nodeId)
{
    pthread_mutex_lock(&lock);
    NodeState *node = findNode(nodeId, true);
    if (node != NULL) {
        node->lastSeenMs = nowMs();
    }
    pthread_mutex_unlock(&lock);
}

void Dashboard_recordEvent(uint16_t nodeId, float thetaDeg, float confidence, uint16_t flags)
{
    pthread_mutex_lock(&lock);
    NodeState *node = findNode(nodeId, true);
    if (node != NULL) {
        int64_t now = nowMs();
        node->thetaDeg = thetaDeg;
        node->confidence = confidence;
        node->flags = flags;
        node->eventCount++;
        node->lastEventMs = now;
        node->lastSeenMs = now;
    }
    pthread_mutex_unlock(&lock);
}

void Dashboard_addFix(const DashboardFix *fix)
{
    pthread_mutex_lock(&lock);
    StoredFix *slot = &fixes[fixHead];
    slot->fix = *fix;
    if (slot->fix.numBearings > DASHBOARD_MAX_NODES) {
        slot->fix.numBearings = DASHBOARD_MAX_NODES;
    }
    slot->id = nextFixId++;
    slot->timeMs = nowMs();
    fixHead = (fixHead + 1) % DASHBOARD_FIX_HISTORY;
    if (fixCount < DASHBOARD_FIX_HISTORY) {
        fixCount++;
    }
    pthread_mutex_unlock(&lock);
}

/**
 * @brief snprintf that appends at *used and remembers if anything overflowed
 */
static void append(char *buffer, size_t size, size_t *used, bool *isTruncated, const char *format, ...)
    __attribute__((format(printf, 5, 6)));

static void append(char *buffer, size_t size, size_t *used, bool *isTruncated, const char *format, ...)
{
    if (*isTruncated) {
        return;
    }
    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer + *used, size - *used, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= size - *used) {
        *isTruncated = true;
        return;
    }
    *used += (size_t)written;
}

/**
 * @brief Appends one fix as JSON. Caller holds the lock.
 */
static void appendFix(char *buffer, size_t size, size_t *used, bool *isTruncated,
                      const StoredFix *stored, int64_t now, bool isFirst)
{
    const DashboardFix *fix = &stored->fix;
    append(buffer, size, used, isTruncated, "%s{\"id\":%u,\"age_ms\":%lld,\"bearings\":[",
           isFirst ? "" : ",", (unsigned)stored->id, (long long)(now - stored->timeMs));
    for (int b = 0; b < fix->numBearings; b++) {
        const DashboardBearing *bearing = &fix->bearings[b];
        append(buffer, size, used, isTruncated,
               "%s{\"node\":%u,\"theta\":%.2f,\"bearing\":%.2f,\"confidence\":%.2f}",
               b == 0 ? "" : ",", (unsigned)bearing->nodeId, bearing->thetaDeg,
               bearing->bearingDeg, bearing->confidence);
    }
    if (fix->hasPosition) {
        append(buffer, size, used, isTruncated,
               "],\"position\":{\"x\":%.3f,\"y\":%.3f,\"rms\":%.3f,\"in_front\":%s}}",
               fix->x, fix->y, fix->rmsM, fix->isInFront ? "true" : "false");
    } else {
        append(buffer, size, used, isTruncated, "],\"position\":null}");
    }
}

bool Dashboard_toJson(char *buffer, size_t size)
{
    if (size < sizeof(EMPTY_JSON)) {
        return false;
    }
    size_t used = 0;
    bool isTruncated = false;

    pthread_mutex_lock(&lock);
    int64_t now = nowMs();
    append(buffer, size, &used, &isTruncated,
           "{\"uptime_ms\":%lld,\"demo\":%s,\"nodes\":[",
           (long long)(now - startMs), isDemoData ? "true" : "false");

    bool isFirst = true;
    for (int i = 0; i < DASHBOARD_MAX_NODES; i++) {
        const NodeState *node = &nodes[i];
        if (!node->isUsed) {
            continue;
        }
        append(buffer, size, &used, &isTruncated,
               "%s{\"id\":%u,\"x\":%.3f,\"y\":%.3f,\"heading\":%.1f,\"connected\":%s,"
               "\"seen_ms\":%lld,\"events\":%u,\"theta\":%.2f,\"confidence\":%.2f,"
               "\"flags\":%u,\"event_ms\":%lld}",
               isFirst ? "" : ",", (unsigned)node->id, node->x, node->y, node->headingDeg,
               node->isConnected ? "true" : "false", (long long)ageMs(node->lastSeenMs, now),
               (unsigned)node->eventCount, node->thetaDeg, node->confidence,
               (unsigned)node->flags, (long long)ageMs(node->lastEventMs, now));
        isFirst = false;
    }

    append(buffer, size, &used, &isTruncated, "],\"fixes\":[");
    for (int i = 0; i < fixCount; i++) {
        // Newest first: step back from the head
        int index = (fixHead - 1 - i + DASHBOARD_FIX_HISTORY) % DASHBOARD_FIX_HISTORY;
        appendFix(buffer, size, &used, &isTruncated, &fixes[index], now, i == 0);
    }
    append(buffer, size, &used, &isTruncated, "]}");
    pthread_mutex_unlock(&lock);

    if (isTruncated) {
        snprintf(buffer, size, EMPTY_JSON);
        return false;
    }
    return true;
}
