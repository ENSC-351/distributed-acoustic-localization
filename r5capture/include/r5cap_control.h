// r5cap_control.h
// Linux -> R5 control of the capture firmware through shared memory, so starting,
// stopping and changing the SPI clock never needs the R5's shell (which shares the
// CPU with the capture loop, and whose RPMsg tty can block a script).
//
// Lives in the heartbeat page (last 4 KB of the r5capture region).
// Protocol:
//   R5 at boot:  clear everything, barrier, magic = R5CAP_CONTROL_MAGIC
//   Linux:       wait for magic; write rateHz, spiHz; barrier; seq = seq + 1
//   R5:          notices seq changed (once per block while sampling, every 10 ms
//                when idle); stops, applies the clock (if spiHz != 0), starts at
//                rateHz (0 = stay stopped); writes ackSpiHz; barrier; ackSeq = seq
//   Linux:       waits until ackSeq == seq
#ifndef R5CAP_CONTROL_H
#define R5CAP_CONTROL_H

#include "r5cap_heartbeat.h"
#include <stdint.h>

#define R5CAP_CONTROL_PHYS (R5CAP_HEARTBEAT_PHYS + 0x100u)
#define R5CAP_CONTROL_MAGIC 0x4C544335u   // "5CTL"

struct r5cap_control {
    volatile uint32_t magic;      // set by the R5 once it accepts commands
    volatile uint32_t seq;        // Linux: +1 per command
    volatile uint32_t rateHz;     // frames per second per mic; 0 = stop
    volatile uint32_t spiHz;      // requested SPI clock; 0 = keep the current one
    volatile uint32_t ackSeq;     // R5: seq of the last command applied
    volatile uint32_t ackSpiHz;   // R5: SPI clock actually set (divider result)
    volatile uint32_t ackRateHz;  // R5: rate now running (0 = stopped)
};

#endif
