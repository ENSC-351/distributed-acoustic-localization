// Entry point for the master executable
//
// Usage: master [--demo]
//   --demo  fake sounds every few seconds, located by the real fusion code
//           (there is no node networking yet)
//
// Runs the UDP server for the web GUI (port 12345) until "stop" arrives over
// UDP or Ctrl+C is pressed.
#include "dashboard.h"
#include "demo_source.h"
#include "fusion.h"
#include "layout.h"
#include "stop.h"
#include "udp.h"
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief Ctrl+C handler: asks main() to shut down cleanly
 */
static void onInterrupt(int signalNumber)
{
    (void)signalNumber;
    Stop_request();
}

/**
 * @brief Routes SIGINT and SIGTERM to Stop_request()
 */
static void installSignalHandlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = onInterrupt;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
}

int main(int argc, char *argv[])
{
    bool isDemo = (argc > 1 && strcmp(argv[1], "--demo") == 0);

    Stop_init();
    installSignalHandlers();
    Dashboard_init(isDemo);
    for (int i = 0; i < LAYOUT_NUM_NODES; i++) {
        const LayoutNode *node = Layout_getByIndex(i);
        Dashboard_setNodePose(node->id, node->x, node->y, node->headingDeg);
    }
    Fusion_init();
    UDP_init();
    if (isDemo) {
        DemoSource_init();
    }
    printf("Master running%s. Web GUI data on UDP port %d. Ctrl+C to stop.\n",
           isDemo ? " with demo data" : "", UDP_PORT);

    Stop_waitForRequest();
    printf("Master stopping.\n");

    DemoSource_cleanup();
    UDP_cleanup();
    Fusion_cleanup();
    Dashboard_cleanup();
    Stop_cleanup();
    return 0;
}
