/**
 * @file dashboard.h
 * @author Madden Wynans
 * @brief Thread-safe snapshot of what the master knows, for the web GUI
 * @details Networking (or the demo source) and fusion write into it; the UDP
 *          server reads it as JSON. Coordinates and angles follow layout.h.
 *
 *          A "fix" is one sound: the direction lines the nodes reported for it
 *          and, if at least two lines crossed, the estimated position. The last
 *          DASHBOARD_FIX_HISTORY fixes are kept, newest first in the JSON.
 */
#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DASHBOARD_MAX_NODES 8
#define DASHBOARD_FIX_HISTORY 8

/**
 * @brief One node's direction line within a fix
 */
typedef struct {
    uint16_t nodeId;
    float thetaDeg;     // as the node reported it
    float bearingDeg;   // on the map (layout.h)
    float confidence;
} DashboardBearing;

/**
 * @brief One sound, as fused from the nodes' reports
 */
typedef struct {
    int numBearings;
    DashboardBearing bearings[DASHBOARD_MAX_NODES];
    bool hasPosition;   // false if fewer than 2 lines or they were parallel
    float x;
    float y;
    float rmsM;         // spread: RMS distance from the point to the lines
    bool isInFront;     // point is in front of every node that reported
} DashboardFix;

/**
 * @brief Initializes the dashboard (no nodes, no fixes)
 * @param isDemo true if the data is synthetic, shown as a badge on the page
 */
void Dashboard_init(bool isDemo);

/**
 * @brief Frees the dashboard
 */
void Dashboard_cleanup(void);

/**
 * @brief Adds a node or updates where it is placed
 * @param headingDeg node broadside direction, degrees CCW from +x
 * @return false if the table is full or nodeId is 0
 */
bool Dashboard_setNodePose(uint16_t nodeId, float x, float y, float headingDeg);

/**
 * @brief Marks a node connected or disconnected
 */
void Dashboard_setNodeConnected(uint16_t nodeId, bool isConnected);

/**
 * @brief Records that a message (any type) arrived from a node
 */
void Dashboard_touchNode(uint16_t nodeId);

/**
 * @brief Records a node's latest direction estimate (for the node table)
 * @param thetaDeg as reported by the node
 * @param confidence 0.0 to 1.0
 * @param flags PROTO_EVENT_* flags as received
 */
void Dashboard_recordEvent(uint16_t nodeId, float thetaDeg, float confidence, uint16_t flags);

/**
 * @brief Adds a fix to the history; the dashboard numbers and timestamps it
 */
void Dashboard_addFix(const DashboardFix *fix);

/**
 * @brief Writes the whole state as one line of JSON
 * @param buffer output, always '\0' terminated
 * @param size size of buffer in bytes
 * @return false if the JSON did not fit (buffer then holds "{}")
 */
bool Dashboard_toJson(char *buffer, size_t size);

#endif
