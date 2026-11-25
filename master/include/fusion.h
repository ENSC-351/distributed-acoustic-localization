/**
 * @file fusion.h
 * @author Madden Wynans
 * @brief Groups the nodes' reports of one sound and locates it
 * @details Networking calls Fusion_addEvent() for every EVENT it receives.
 *          Reports that arrive within FUSION_WINDOW_MS of the first one are
 *          treated as the same sound. The group closes when every node in the
 *          layout has reported, when a node reports twice, or when the window
 *          runs out. A closed group becomes one fix in the dashboard, with a
 *          position if at least two lines crossed (locate.h).
 *
 *          Grouping uses the time the master *receives* each report, not the
 *          nodes' t_event_ns, so it works without synchronised clocks. Sound
 *          crosses a 2 m triangle in ~6 ms, so the window mostly covers
 *          detection and network delay.
 */
#ifndef FUSION_H
#define FUSION_H

#include <stdint.h>

#define FUSION_WINDOW_MS 150

/**
 * @brief Starts the thread that closes groups when their window runs out
 * @warning Call Dashboard_init() first
 */
void Fusion_init(void);

/**
 * @brief Closes any open group and stops the thread
 */
void Fusion_cleanup(void);

/**
 * @brief Adds one node's direction report
 * @param nodeId must be in the layout (others are counted in the node table only)
 * @param thetaDeg as reported by the node (tdoa.h convention)
 * @param confidence 0.0 to 1.0; used as the line's weight
 * @param flags PROTO_EVENT_* flags as received
 */
void Fusion_addEvent(uint16_t nodeId, float thetaDeg, float confidence, uint16_t flags);

#endif
