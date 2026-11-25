// r5cap_control.h
// Linux -> R5 control of the capture firmware through shared memory, so starting,
// stopping and changing the SPI clock never needs the R5's shell (which shares the
// CPU with the capture loop, and whose RPMsg tty can block a script).
//
// Lives in the heartbeat page (last 4 KB of the r5capture region).
// Protocol:
//   R5 at boot:  clear everything, barrier, magic = R5CAP_CONTROL_MAGIC
//   Linux:       wait for magic; write rateHz, spiHz, flags; barrier; seq = seq + 1
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
    volatile uint32_t flags;      // Linux: R5CAP_CTL_* for the next start
    volatile uint32_t ackFlags;   // R5: flags now in effect
};

#define R5CAP_CTL_IRQ_LOCK (1u << 0)   // interrupts off on the R5 while sampling
#define R5CAP_CTL_PROFILE (1u << 1)    // time each part of a frame (first R5CAP_PROFILE_FRAMES frames)
#define R5CAP_PROFILE_FRAMES 8192

// Diagnostics written by the R5 while sampling (reset at each start): how late the
// frame loop gets, and when, to find what delays it.
#define R5CAP_DIAG_PHYS (R5CAP_CONTROL_PHYS + 0x100u)
#define R5CAP_DIAG_EVENTS 32

struct r5cap_diag {
    volatile uint32_t lateEvents;          // frames that started > half a period late
    volatile uint32_t maxOvershootTicks;   // worst lateness of a frame start, in timer ticks
    volatile uint32_t eventCount;          // events recorded so far (ring index = count % EVENTS)
    volatile uint32_t eventTick[R5CAP_DIAG_EVENTS];      // low 32 bits of the timer at the event
    volatile uint32_t eventOvershoot[R5CAP_DIAG_EVENTS]; // how late, in ticks
    // Profile (R5CAP_CTL_PROFILE), sums in timer ticks over profFrames frames
    volatile uint32_t profFrames;         // frames profiled
    volatile uint32_t profFrameTicks;     // frame start to next frame start
    volatile uint32_t profTickTicks;      // inside CAPTURE_tick() (conversions + bookkeeping)
    volatile uint32_t profSpiTicks;       // inside R5HAL_spiTransfer() (both conversions)
    volatile uint32_t profShiftTicks;     // from TX write to RX word ready (bit shifting)
    volatile uint32_t profBlockEnds;      // frames that ended a block (publish + control check)
    volatile uint32_t profBlockEndTicks;  // CAPTURE_tick() time of those frames
    volatile uint32_t profMaxTickTicks;   // longest CAPTURE_tick()
};

// Written by the R5's fatal-error handler (zephyr_app/src/fatal.c) when the firmware
// crashes, so Linux can see why even though the R5 has stopped. Cleared at boot.
#define R5CAP_CRASH_PHYS (R5CAP_HEARTBEAT_PHYS + 0x800u)
#define R5CAP_CRASH_MAGIC 0x48535243u   // "CRSH"

struct r5cap_crash {
    volatile uint32_t magic;    // R5CAP_CRASH_MAGIC once a crash was recorded, 0 at boot
    volatile uint32_t reason;   // Zephyr K_ERR_* reason
    volatile uint32_t pc;       // where it happened (exception frame)
    volatile uint32_t lr;
    volatile uint32_t dfsr;     // data fault status / address (CP15)
    volatile uint32_t dfar;
    volatile uint32_t ifsr;     // instruction fault status / address (CP15)
    volatile uint32_t ifar;
    volatile uint32_t r0;
    volatile uint32_t xpsr;
};

#endif
