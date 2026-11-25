#include "locate.h"
#include <math.h>

#define PI 3.14159265358979323846
#define RAD_PER_DEG (PI / 180.0)
#define MIN_LINES 2
// Lines within about 1 degree of parallel have no useful crossing
#define MIN_DETERMINANT 1e-4

bool Locate_intersect(const LocateLine *lines, int numLines, LocateResult *result)
{
    if (numLines < MIN_LINES) {
        return false;
    }

    // Line i: points p with n_i . p = n_i . p_i, where n_i = (-sin b, cos b) is
    // the line's normal. Minimising sum w_i (n_i . p - n_i . p_i)^2 gives the
    // 2x2 system  A p = r  with  A = sum w n n^T  and  r = sum w n (n . p_i).
    double a11 = 0, a12 = 0, a22 = 0, r1 = 0, r2 = 0, totalWeight = 0;
    for (int i = 0; i < numLines; i++) {
        double bearing = lines[i].bearingDeg * RAD_PER_DEG;
        double nx = -sin(bearing);
        double ny = cos(bearing);
        double w = lines[i].weight;
        double offset = nx * lines[i].x + ny * lines[i].y;
        a11 += w * nx * nx;
        a12 += w * nx * ny;
        a22 += w * ny * ny;
        r1 += w * nx * offset;
        r2 += w * ny * offset;
        totalWeight += w;
    }

    if (totalWeight <= 0) {
        return false;
    }
    // Divide by totalWeight^2 so the parallel-lines test doesn't depend on the
    // weights' scale (for two equal-weight lines it is sin^2(angle between)/4)
    double determinant = a11 * a22 - a12 * a12;
    if (determinant / (totalWeight * totalWeight) < MIN_DETERMINANT) {
        return false;
    }
    double x = (a22 * r1 - a12 * r2) / determinant;
    double y = (a11 * r2 - a12 * r1) / determinant;

    double sumSquares = 0;
    bool isInFront = true;
    for (int i = 0; i < numLines; i++) {
        double bearing = lines[i].bearingDeg * RAD_PER_DEG;
        double dx = x - lines[i].x;
        double dy = y - lines[i].y;
        double along = dx * cos(bearing) + dy * sin(bearing);
        double across = -dx * sin(bearing) + dy * cos(bearing);
        sumSquares += lines[i].weight * across * across;
        if (along < 0) {
            isInFront = false;
        }
    }

    result->x = (float)x;
    result->y = (float)y;
    result->rmsM = (float)sqrt(sumSquares / totalWeight);
    result->isInFront = isInFront;
    return true;
}
