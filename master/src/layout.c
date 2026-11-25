#include "layout.h"
#include <stddef.h>

// Circumradius of an equilateral triangle = side / sqrt(3)
#define SQRT3 1.7320508f
#define CIRCUMRADIUS_M (LAYOUT_SIDE_M / SQRT3)
#define COS30 0.8660254f
#define SIN30 0.5f

static const LayoutNode nodes[LAYOUT_NUM_NODES] = {
    // id  x                          y                         heading (toward centre)
    {1,  0.0f,                        CIRCUMRADIUS_M,           -90.0f},
    {2, -CIRCUMRADIUS_M * COS30,     -CIRCUMRADIUS_M * SIN30,    30.0f},
    {3,  CIRCUMRADIUS_M * COS30,     -CIRCUMRADIUS_M * SIN30,   150.0f},
};

const LayoutNode *Layout_getByIndex(int index)
{
    if (index < 0 || index >= LAYOUT_NUM_NODES) {
        return NULL;
    }
    return &nodes[index];
}

const LayoutNode *Layout_getById(uint16_t nodeId)
{
    for (int i = 0; i < LAYOUT_NUM_NODES; i++) {
        if (nodes[i].id == nodeId) {
            return &nodes[i];
        }
    }
    return NULL;
}

float Layout_wrapDeg(float angleDeg)
{
    while (angleDeg > 180.0f) {
        angleDeg -= 360.0f;
    }
    while (angleDeg <= -180.0f) {
        angleDeg += 360.0f;
    }
    return angleDeg;
}

float Layout_thetaToBearingDeg(const LayoutNode *node, float thetaDeg)
{
    return Layout_wrapDeg(node->headingDeg - thetaDeg);
}

float Layout_bearingToThetaDeg(const LayoutNode *node, float bearingDeg)
{
    return Layout_wrapDeg(node->headingDeg - bearingDeg);
}
