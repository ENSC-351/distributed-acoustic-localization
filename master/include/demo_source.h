/**
 * @file demo_source.h
 * @author Madden Wynans
 * @brief Fake sounds for the web GUI until the master's networking exists
 * @details Every DEMO_PERIOD_MS (or on DemoSource_trigger()) a pretend sound
 *          happens at a random spot, mostly inside the node triangle. Each node
 *          in the layout "hears" it: its theta gets a few degrees of noise, its
 *          report arrives after a random network delay, and now and then a node
 *          misses the sound. Reports go through Fusion_addEvent(), the same path
 *          real EVENT messages will take, so the triangulation is real; only the
 *          input is fake. The true positions are printed to stdout so the
 *          estimates can be compared with them.
 */
#ifndef DEMO_SOURCE_H
#define DEMO_SOURCE_H

#include <stdbool.h>

#define DEMO_PERIOD_MS 3000

/**
 * @brief Marks the layout nodes connected and starts the demo thread
 * @warning Call Dashboard_init() and Fusion_init() first
 */
void DemoSource_init(void);

/**
 * @brief Stops and joins the demo thread
 */
void DemoSource_cleanup(void);

/**
 * @brief Makes a sound happen now instead of waiting for the next period
 * @return false if the demo source isn't running
 */
bool DemoSource_trigger(void);

#endif
