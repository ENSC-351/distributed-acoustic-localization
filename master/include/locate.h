/**
 * @file locate.h
 * @author Madden Wynans
 * @brief Finds the source position from two or more direction lines
 * @details Each node gives a line on the map: it starts at the node and points
 *          along the bearing. Locate_intersect() returns the point with the
 *          smallest weighted sum of squared perpendicular distances to all the
 *          lines (least squares). With exactly two lines that is their crossing.
 *          Pure math, no state and no threads, so it is easy to unit-test.
 */
#ifndef LOCATE_H
#define LOCATE_H

#include <stdbool.h>

/**
 * @brief One direction line
 */
typedef struct {
    float x;            // where the line starts (node position), metres
    float y;
    float bearingDeg;   // map bearing, degrees CCW from +x
    float weight;       // > 0; the node's confidence
} LocateLine;

/**
 * @brief Result of an intersection
 */
typedef struct {
    float x;
    float y;
    float rmsM;         // weighted RMS distance from the point to the lines, metres
    bool isInFront;     // point is in front of every node (not behind any of them)
} LocateResult;

/**
 * @brief Least-squares intersection of direction lines
 * @param lines at least 2 lines
 * @param numLines number of lines
 * @param result filled in only when the function returns true
 * @return false if there are fewer than 2 lines or they are (nearly) parallel
 */
bool Locate_intersect(const LocateLine *lines, int numLines, LocateResult *result);

#endif
