/**
 * @file layout.h
 * @author Madden Wynans
 * @brief Where the nodes sit: an equilateral triangle, every node facing the centre
 * @details Map coordinates are metres with the origin at the triangle's centre,
 *          x right, y up. Angles on the map are degrees counter-clockwise from +x.
 *
 *          Node 1 is at the top, then 2 (bottom left) and 3 (bottom right).
 *          A node's heading is its broadside direction (straight out of the
 *          front of the mic pair); here it points at the centre.
 *
 *          The DSP estimator (dsp/include/tdoa.h) reports theta in degrees:
 *          0 = broadside, positive toward mic B, front half-plane only.
 *          Seen from above with the node facing up, mic B is on the right,
 *          so positive theta turns clockwise:  map bearing = heading - theta.
 *          Mount every node with mic B on its right when facing the centre.
 */
#ifndef LAYOUT_H
#define LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#define LAYOUT_MAX_NODES 8   // most nodes a layout file can list
#define LAYOUT_SIDE_M 2.0f  // built-in triangle only: distance between nodes (use a layout file for the real setup)

/**
 * @brief One node's place on the map
 */
typedef struct {
    uint16_t id;
    float x;
    float y;
    float headingDeg;
} LayoutNode;

/**
 * @brief Returns the node at index 0 .. Layout_count()-1
 */
const LayoutNode *Layout_getByIndex(int index);

/**
 * @brief Returns the node with this protocol node_id, or NULL if it isn't in the layout
 */
const LayoutNode *Layout_getById(uint16_t nodeId);

/**
 * @brief Converts a node's reported theta to a bearing on the map
 * @return degrees CCW from +x, wrapped to (-180, 180]
 */
float Layout_thetaToBearingDeg(const LayoutNode *node, float thetaDeg);

/**
 * @brief Inverse of Layout_thetaToBearingDeg() (used by the demo source)
 * @return theta in degrees, wrapped to (-180, 180]
 */
float Layout_bearingToThetaDeg(const LayoutNode *node, float bearingDeg);

/**
 * @brief Wraps an angle in degrees to (-180, 180]
 */
float Layout_wrapDeg(float angleDeg);

/**
 * @brief Loads node positions from a text file, replacing the built-in triangle.
 *        One node per line: "<id> <x m> <y m> <heading deg>"; '#' starts a comment.
 *        Heading is the way the node faces (its 0 degrees), counter-clockwise from +x.
 * @return false (and keeps the built-in triangle) if the file can't be read or has no nodes
 */
bool Layout_load(const char *path);

/**
 * @brief Number of nodes in the layout
 */
int Layout_count(void);

#endif
