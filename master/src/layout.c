#include "layout.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Circumradius of an equilateral triangle = side / sqrt(3)
#define SQRT3 1.7320508f
#define CIRCUMRADIUS_M (LAYOUT_SIDE_M / SQRT3)
#define COS30 0.8660254f
#define SIN30 0.5f

// Built-in layout: equilateral triangle, every node facing the centre.
// A layout file (Layout_load) replaces it.
static LayoutNode nodes[LAYOUT_MAX_NODES] = {
    // id  x                          y                         heading (toward centre)
    {1,  0.0f,                        CIRCUMRADIUS_M,           -90.0f},
    {2, -CIRCUMRADIUS_M * COS30,     -CIRCUMRADIUS_M * SIN30,    30.0f},
    {3,  CIRCUMRADIUS_M * COS30,     -CIRCUMRADIUS_M * SIN30,   150.0f},
};
static int nodeCount = 3;

bool Layout_load(const char *path)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        perror(path);
        return false;
    }
    LayoutNode loaded[LAYOUT_MAX_NODES];
    int count = 0;
    int lineNumber = 0;
    bool isOk = true;
    char line[256];
    while (fgets(line, sizeof(line), file) != NULL) {
        lineNumber++;
        char *comment = strchr(line, '#');
        if (comment != NULL) {
            *comment = '\0';
        }
        unsigned id;
        float x, y, heading;
        char extra;
        int fields = sscanf(line, " %u %f %f %f %c", &id, &x, &y, &heading, &extra);
        if (fields == EOF || (fields <= 0 && strspn(line, " \t\r\n") == strlen(line))) {
            continue;   // blank line
        }
        if (fields != 4 || id == 0 || id > UINT16_MAX) {
            fprintf(stderr, "%s:%d: expected \"<id> <x m> <y m> <heading deg>\"\n", path, lineNumber);
            isOk = false;
            break;
        }
        if (count == LAYOUT_MAX_NODES) {
            fprintf(stderr, "%s: more than %d nodes\n", path, LAYOUT_MAX_NODES);
            isOk = false;
            break;
        }
        loaded[count].id = (uint16_t)id;
        loaded[count].x = x;
        loaded[count].y = y;
        loaded[count].headingDeg = Layout_wrapDeg(heading);
        count++;
    }
    fclose(file);
    if (!isOk || count == 0) {
        fprintf(stderr, "%s: not used, keeping the built-in layout\n", path);
        return false;
    }
    memcpy(nodes, loaded, sizeof(loaded[0]) * (size_t)count);
    nodeCount = count;
    return true;
}

int Layout_count(void)
{
    return nodeCount;
}

const LayoutNode *Layout_getByIndex(int index)
{
    if (index < 0 || index >= nodeCount) {
        return NULL;
    }
    return &nodes[index];
}

const LayoutNode *Layout_getById(uint16_t nodeId)
{
    for (int i = 0; i < nodeCount; i++) {
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
