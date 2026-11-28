#include "demo_source.h"
#include "dashboard.h"
#include "fusion.h"
#include "layout.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PI 3.14159265358979323846
#define DEG_PER_RAD (180.0 / PI)
#define NS_PER_MS 1000000
#define SLEEP_STEP_MS 50

#define NOISE_DEG 3.0
#define MAX_DELAY_MS 40         // network + detection delay per report
#define MISS_PERCENT 10         // chance a node misses a sound
#define INSIDE_PERCENT 80       // sounds inside the triangle; the rest nearby
#define NEARBY_RADIUS_M 1.6
#define MIN_CONFIDENCE 0.6
#define DEMO_SEED 351u

static pthread_t demoThread;
static bool isInitialized = false;
static atomic_bool isRunning = false;
static atomic_bool isTriggered = false;
static unsigned int seed = DEMO_SEED;   // only the demo thread uses it

/**
 * @brief Uniform random number in [0, 1]
 */
static double random01(void)
{
    return (double)rand_r(&seed) / RAND_MAX;
}

/**
 * @brief Picks where the pretend sound happens
 */
static void pickSource(double *x, double *y)
{
    if (Layout_count() >= 3 && rand_r(&seed) % 100 < INSIDE_PERCENT) {
        // Uniform inside the triangle: fold the unit square onto it
        double u = random01();
        double v = random01();
        if (u + v > 1.0) {
            u = 1.0 - u;
            v = 1.0 - v;
        }
        const LayoutNode *a = Layout_getByIndex(0);
        const LayoutNode *b = Layout_getByIndex(1);
        const LayoutNode *c = Layout_getByIndex(2);
        *x = a->x + u * (b->x - a->x) + v * (c->x - a->x);
        *y = a->y + u * (b->y - a->y) + v * (c->y - a->y);
    } else {
        double angle = random01() * 2.0 * PI;
        double radius = NEARBY_RADIUS_M * sqrt(random01());
        *x = radius * cos(angle);
        *y = radius * sin(angle);
    }
}

/**
 * @brief Theta a two-mic node would report for a sound at (x, y)
 * @details The pair can't tell front from back, so a sound behind the node
 *          comes out mirrored into the front half-plane, like tdoa.h's output.
 */
static float reportedTheta(const LayoutNode *node, double x, double y)
{
    double bearing = atan2(y - node->y, x - node->x) * DEG_PER_RAD;
    double theta = Layout_bearingToThetaDeg(node, (float)bearing);
    if (theta > 90.0) {
        theta = 180.0 - theta;
    } else if (theta < -90.0) {
        theta = -180.0 - theta;
    }
    return (float)(theta + (random01() * 2.0 - 1.0) * NOISE_DEG);
}

static void sleepMs(int ms)
{
    struct timespec duration = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * NS_PER_MS};
    nanosleep(&duration, NULL);
}

/**
 * @brief One pretend sound: every node that hears it reports after its own delay
 */
static void makeSound(void)
{
    double x, y;
    pickSource(&x, &y);

    int delayMs[LAYOUT_MAX_NODES];
    bool hears[LAYOUT_MAX_NODES];
    int numHearing = 0;
    for (int i = 0; i < Layout_count(); i++) {
        delayMs[i] = rand_r(&seed) % (MAX_DELAY_MS + 1);
        hears[i] = (rand_r(&seed) % 100 >= MISS_PERCENT);
        numHearing += hears[i] ? 1 : 0;
    }
    if (numHearing == 0) {
        hears[0] = true;
        numHearing = 1;
    }
    printf("demo: sound at (%.2f, %.2f) m, heard by %d node(s)\n", x, y, numHearing);
    fflush(stdout);

    // Deliver in order of delay
    int elapsedMs = 0;
    for (int sent = 0; sent < Layout_count(); sent++) {
        int next = -1;
        for (int i = 0; i < Layout_count(); i++) {
            if (delayMs[i] >= 0 && (next < 0 || delayMs[i] < delayMs[next])) {
                next = i;
            }
        }
        sleepMs(delayMs[next] - elapsedMs);
        elapsedMs = delayMs[next];
        delayMs[next] = -1;
        if (hears[next]) {
            const LayoutNode *node = Layout_getByIndex(next);
            float confidence = (float)(MIN_CONFIDENCE + random01() * (1.0 - MIN_CONFIDENCE));
            Fusion_addEvent(node->id, reportedTheta(node, x, y), confidence, 0);
        }
    }
}

/**
 * @brief Demo thread: a sound every DEMO_PERIOD_MS or when triggered
 */
static void *demoThreadFn(void *arg)
{
    (void)arg;
    int waitedMs = DEMO_PERIOD_MS;  // first sound right away
    while (atomic_load(&isRunning)) {
        if (waitedMs >= DEMO_PERIOD_MS || atomic_exchange(&isTriggered, false)) {
            makeSound();
            waitedMs = 0;
        }
        for (int i = 0; i < Layout_count(); i++) {
            Dashboard_touchNode(Layout_getByIndex(i)->id);  // stand-in for heartbeats
        }
        sleepMs(SLEEP_STEP_MS);
        waitedMs += SLEEP_STEP_MS;
    }
    return NULL;
}

void DemoSource_init(void)
{
    if (isInitialized) {
        return;
    }
    for (int i = 0; i < Layout_count(); i++) {
        Dashboard_setNodeConnected(Layout_getByIndex(i)->id, true);
    }
    atomic_store(&isRunning, true);
    int err = pthread_create(&demoThread, NULL, demoThreadFn, NULL);
    if (err != 0) {
        fprintf(stderr, "Unable to create demo thread: %s\n", strerror(err));
        exit(EXIT_FAILURE);
    }
    isInitialized = true;
}

void DemoSource_cleanup(void)
{
    if (!isInitialized) {
        return;
    }
    atomic_store(&isRunning, false);
    pthread_join(demoThread, NULL);
    isInitialized = false;
}

bool DemoSource_trigger(void)
{
    if (!atomic_load(&isRunning)) {
        return false;
    }
    atomic_store(&isTriggered, true);
    return true;
}
