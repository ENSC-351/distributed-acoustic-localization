// capture_main.c
// Runs the capture core (fw/capture.c) on the R5: samples MCP3208 ch0 and ch1
// every frame period into the shared-memory ring at R5CAP_SHM_PHYS, which Linux
// reads with linux/r5cap_ring_read (via linux/r5cap_reader.c).
//
// Control: linux/r5cap_ctl writes commands into shared memory (r5cap_control.h),
// checked once per block; the shell commands below work while capture is stopped.
// Autostart: the bit-banged build starts at AUTOSTART_RATE_HZ; the McSPI build
// waits for "r5cap start" (the board script first checks MCU_SPI0 is powered:
// touching an unpowered module can fault the R5).
// Zephyr shell over RPMsg (/dev/ttyRPMSG0):
//   r5cap start [frames_per_s]   (re)start capture; resets the ring
//   r5cap stop
//   r5cap stats                  ring header: state, blocks, late ticks, ADC errors
//   r5cap spiclk <hz>            set the SPI clock (McSPI build; capture stopped)
//   r5cap spitest                time conversions, measure the real SPI clock (stopped)
//
// Frame timing: a busy-wait on the 25 MHz system timer (simplest first version;
// a hardware timer interrupt can replace it later without touching capture.c).
// The thread runs at the lowest preemptible priority and never yields while
// sampling: RPMsg (cooperative) preempts it only when Linux sends something, the
// heartbeat is paused, and the shell (same priority) only runs while capture is
// stopped. While sampling, control it from Linux with linux/r5cap_ctl (shared memory). Anything that delays a frame by more
// than half a period is counted by capture.c as a late tick and flags the block.
// (A k_yield() per block cost a context switch each time: ~1 late frame per 3 blocks
// above 14,000 frames/s on the board.)
#include "capture.h"
#include "hal_r5_spi.h"
#include "mcp3208.h"
#include "r5_hal.h"
#include "r5cap_fastmem.h"
#include "r5cap_control.h"
#include "r5cap_heartbeat.h"
#include <zephyr/sys/barrier.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <stdlib.h>

#define DEFAULT_RATE_HZ 16000
#define MAX_RATE_HZ 40000
#ifndef AUTOSTART_RATE_HZ
#define AUTOSTART_RATE_HZ 0          // set by CMakeLists.txt: 4000 for the gpio build
#endif
#define WORD19_BITS 19
#define BLOCK_FRAMES 128
#define RING_BLOCKS 256
#define CAPTURE_STACK_SIZE 2048
#define CAPTURE_PRIORITY 14          // lowest preemptible, same as the shell: while sampling the
                                     // loop never yields, so use r5cap_ctl (shared memory) then
#define IDLE_POLL_MS 10
#define INIT_RETRY_MS 1000
#define SPITEST_CONVERSIONS 200

// Ring at the start of the shared region; the heartbeat keeps the last 4 KB
#define RING_BYTES (R5CAP_HEARTBEAT_PHYS - R5CAP_SHM_PHYS)
static uint8_t *const ring = (uint8_t *)R5CAP_SHM_PHYS;
BUILD_ASSERT(R5CAP_HEADER_SIZE + RING_BLOCKS * (16 + BLOCK_FRAMES * 2 * 2) <= RING_BYTES,
             "ring doesn't fit in the shared region");

static atomic_t requestedRate = ATOMIC_INIT(AUTOSTART_RATE_HZ);   // 0 = stopped
static uint32_t spiClockHz = 0;   // last clock set with "r5cap spiclk" (0 = HAL default)
static atomic_t capturing = ATOMIC_INIT(0);

uint32_t R5_icacheStatus(void);   // icache.c
extern const k_tid_t heartbeat_tid;   // heartbeat.c

// Anything else that wakes up on the R5 while sampling (here: the heartbeat,
// 10 times a second, plus the timer interrupt for its sleep) delays frames.
// Measured: ~11 late frames/s at every rate. So pause it while capturing; the
// ring's writeSeq shows the R5 is alive. With nothing scheduled, the tickless
// kernel has no timer interrupts at all.
static void pauseBackgroundWork(bool pause)
{
    if (pause) {
        k_thread_suspend(heartbeat_tid);
    } else {
        k_thread_resume(heartbeat_tid);
    }
}

// ---- Control from Linux through shared memory (r5cap_control.h) ----
static volatile struct r5cap_control *const control = (struct r5cap_control *)R5CAP_CONTROL_PHYS;
static volatile struct r5cap_diag *const diag = (struct r5cap_diag *)R5CAP_DIAG_PHYS;
static R5CAP_FAST_DATA uint32_t controlSeqSeen = 0;
static R5CAP_FAST_DATA uint32_t captureFlags = 0;   // R5CAP_CTL_* in effect

static void controlInit(void)
{
    control->magic = 0;
    control->seq = 0;
    control->rateHz = 0;
    control->spiHz = 0;
    control->ackSeq = 0;
    control->ackSpiHz = 0;
    control->ackRateHz = 0;
    control->flags = 0;
    control->ackFlags = 0;
    controlSeqSeen = 0;
    barrier_dmem_fence_full();
    control->magic = R5CAP_CONTROL_MAGIC;
}

static inline bool controlPending(void)
{
    return control->seq != controlSeqSeen;
}

// Called with capture stopped: apply a new command from Linux, then acknowledge it
static void controlApply(void)
{
    uint32_t seq = control->seq;
    barrier_dmem_fence_full();   // read the command after seeing the new seq
    uint32_t rate = control->rateHz;
    uint32_t spi = control->spiHz;
    captureFlags = control->flags;
    if (spi != 0) {
        uint32_t actual = R5SPI_setClockHz(spi);
        if (actual != 0) {
            spiClockHz = actual;
        }
    }
    atomic_set(&requestedRate, (rate <= MAX_RATE_HZ) ? (atomic_val_t)rate : 0);
    control->ackSpiHz = spiClockHz;
    control->ackRateHz = (uint32_t)atomic_get(&requestedRate);
    control->ackFlags = captureFlags;
    controlSeqSeen = seq;
    barrier_dmem_fence_full();
    control->ackSeq = seq;
}

static void captureThread(void *unused1, void *unused2, void *unused3)
{
    (void)unused1;
    (void)unused2;
    (void)unused3;

    controlInit();
    for (;;) {
        if (controlPending()) {
            controlApply();
        }
        atomic_val_t rate = atomic_get(&requestedRate);
        if (rate == 0) {
            k_msleep(IDLE_POLL_MS);
            continue;
        }
        const uint32_t period = R5HAL_tickHz() / (uint32_t)rate;
        const struct CaptureConfig config = {
            .numChannels = 2,
            .channels = {0, 1},
            .blockFrames = BLOCK_FRAMES,
            .ringBlocks = RING_BLOCKS,
            .framePeriodTicks = period,
        };
        if (!CAPTURE_init(ring, RING_BYTES, &config)) {
            k_msleep(INIT_RETRY_MS);   // header state shows ERROR
            continue;
        }
        atomic_set(&capturing, 1);
        pauseBackgroundWork(true);

        // Diagnostics: reset for this run
        diag->lateEvents = 0;
        diag->maxOvershootTicks = 0;
        diag->eventCount = 0;
        const uint32_t lateThreshold = period / 2;
        uint32_t maxOvershoot = 0;

        // Optional: no interrupts while sampling. Control is polled from shared
        // memory, so nothing here needs one; RPMsg etc. wait until capture stops.
        const bool lockIrqs = (captureFlags & R5CAP_CTL_IRQ_LOCK) != 0;

        // Optional profile of the first R5CAP_PROFILE_FRAMES frames (sums kept in
        // locals, published at block ends; the probes add a few timer reads per frame)
        bool profiling = (captureFlags & R5CAP_CTL_PROFILE) != 0;
        uint32_t profFrames = 0, profFrameTicks = 0, profTickTicks = 0;
        uint32_t profBlockEnds = 0, profBlockEndTicks = 0, profMaxTick = 0;
        uint64_t profPrevStart = 0;
        diag->profFrames = 0;
        r5spiProfileTicks = 0;
        r5spiProfileShiftTicks = 0;
        r5spiProfileOn = profiling;
        unsigned int irqKey = lockIrqs ? irq_lock() : 0;

        // Hot loop: nothing but waiting for the next frame and sampling it.
        // Commands (shared memory or shell) are checked once per block only.
        uint64_t next = R5HAL_nowTicks();
        uint32_t framesInBlock = 0;
        for (;;) {
            next += period;
            uint64_t now = R5HAL_nowTicks();
            uint32_t missed = 0;
            if (now > next + period) {
                missed = (uint32_t)((now - next) / period);   // whole frame slots lost
                next += (uint64_t)missed * period;            // skip them, keep the schedule
            }
            while (now < next) {
                now = R5HAL_nowTicks();
            }
            uint32_t overshoot = (uint32_t)(now - next);   // how late this frame starts
            if (overshoot > lateThreshold || missed > 0) {
                CAPTURE_markLate(missed);   // slips against the schedule count as late

                uint32_t i = diag->eventCount;
                diag->eventTick[i % R5CAP_DIAG_EVENTS] = (uint32_t)now;
                diag->eventOvershoot[i % R5CAP_DIAG_EVENTS] = overshoot;
                diag->eventCount = i + 1;
                diag->lateEvents = diag->lateEvents + 1;
            }
            if (overshoot > maxOvershoot) {
                maxOvershoot = overshoot;
            }
            CAPTURE_tick();
            if (profiling) {
                uint32_t tick = (uint32_t)(R5HAL_nowTicks() - now);
                if (profPrevStart != 0) {
                    profFrameTicks += (uint32_t)(now - profPrevStart);
                }
                profPrevStart = now;
                profTickTicks += tick;
                profMaxTick = MAX(profMaxTick, tick);
                if (framesInBlock + 1 == BLOCK_FRAMES) {
                    profBlockEnds++;
                    profBlockEndTicks += tick;
                }
                if (++profFrames >= R5CAP_PROFILE_FRAMES) {
                    profiling = false;
                    r5spiProfileOn = false;
                }
            }
            if (++framesInBlock == BLOCK_FRAMES) {
                framesInBlock = 0;
                diag->maxOvershootTicks = maxOvershoot;
                if (profFrames > 0) {
                    diag->profFrameTicks = profFrameTicks;
                    diag->profTickTicks = profTickTicks;
                    diag->profSpiTicks = r5spiProfileTicks;
                    diag->profShiftTicks = r5spiProfileShiftTicks;
                    diag->profBlockEnds = profBlockEnds;
                    diag->profBlockEndTicks = profBlockEndTicks;
                    diag->profMaxTickTicks = profMaxTick;
                    diag->profFrames = profFrames;
                }
                if (controlPending() || atomic_get(&requestedRate) != rate) {
                    break;
                }
            }
        }
        r5spiProfileOn = false;
        if (lockIrqs) {
            irq_unlock(irqKey);
        }
        CAPTURE_stop();
        pauseBackgroundWork(false);
        atomic_set(&capturing, 0);
    }
}

K_THREAD_DEFINE(capture_tid, CAPTURE_STACK_SIZE, captureThread, NULL, NULL, NULL,
                CAPTURE_PRIORITY, 0, 0);

static int cmdStart(const struct shell *sh, size_t argc, char **argv)
{
    long rate = (argc > 1) ? strtol(argv[1], NULL, 10) : DEFAULT_RATE_HZ;
    if (rate < 1 || rate > MAX_RATE_HZ) {
        shell_error(sh, "rate must be 1..%d frames/s", MAX_RATE_HZ);
        return -EINVAL;
    }
    atomic_set(&requestedRate, 0);   // force a restart even at the same rate
    while (atomic_get(&capturing)) {
        k_msleep(1);
    }
    atomic_set(&requestedRate, rate);
    shell_print(sh, "capture starting at %ld frames/s per mic", rate);
    return 0;
}

static int cmdStop(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc;
    (void)argv;
    atomic_set(&requestedRate, 0);
    shell_print(sh, "capture stopping");
    return 0;
}

static int cmdStats(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc;
    (void)argv;
    const struct r5cap_header *h = (const struct r5cap_header *)ring;
    uint32_t sctlr = R5_icacheStatus();
    shell_print(sh, "spi=%s clock=%u Hz (0 = default)  I-cache=%s D-cache=%s", R5SPI_name(),
                (unsigned int)spiClockHz, (sctlr & BIT(12)) ? "on" : "off", (sctlr & BIT(2)) ? "on" : "off");
    shell_print(sh, "ring @0x%08x state=%u writeSeq=%u lateTicks=%u adcErrors=%u",
                (unsigned int)R5CAP_SHM_PHYS, (unsigned int)h->state, (unsigned int)h->writeSeq,
                (unsigned int)h->lateTicks, (unsigned int)h->adcErrors);
    shell_print(sh, "  %u frames/block, %u blocks, period %u ticks @ %u Hz, ch1 offset %u ticks",
                (unsigned int)h->blockFrames, (unsigned int)h->ringBlocks,
                (unsigned int)h->framePeriodTicks, (unsigned int)h->tickHz,
                (unsigned int)h->channelOffsetTicks[1]);
    return 0;
}

static bool isStopped(const struct shell *sh)
{
    if (atomic_get(&requestedRate) != 0 || atomic_get(&capturing)) {
        shell_error(sh, "stop capture first (r5cap stop)");
        return false;
    }
    return true;
}

static int cmdSpiclk(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2 || !isStopped(sh)) {
        return -EINVAL;
    }
    uint32_t actual = R5SPI_setClockHz((uint32_t)strtoul(argv[1], NULL, 10));
    if (actual == 0) {
        shell_error(sh, "%s SPI: clock not adjustable or invalid", R5SPI_name());
        return -EINVAL;
    }
    spiClockHz = actual;
    shell_print(sh, "%s SPI clock set to %u Hz (divider result)", R5SPI_name(), (unsigned int)actual);
    return 0;
}

static int cmdSpitest(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!isStopped(sh)) {
        return -EBUSY;
    }
    const double usPerTick = 1e6 / R5HAL_tickHz();
    double frameUs = 0.0;   // both conversions, back to back
    for (uint8_t ch = 0; ch < 2; ch++) {
        uint32_t minValue = MCP3208_MAX_READING, maxValue = 0, sum = 0;
        uint64_t shiftSum = 0, totalSum = 0;
        int ok = 0;
        for (int i = 0; i < SPITEST_CONVERSIONS; i++) {
            uint16_t v;
            uint32_t shift, total;
            if (!R5SPI_measure(ch, &v, &shift, &total)) {
                continue;
            }
            ok++;
            sum += v;
            shiftSum += shift;
            totalSum += total;
            minValue = MIN(minValue, v);
            maxValue = MAX(maxValue, v);
        }
        if (ok == 0) {
            shell_error(sh, "ch%u: all conversions failed", ch);
            continue;
        }
        double shiftUs = shiftSum * usPerTick / ok;
        double totalUs = totalSum * usPerTick / ok;
        frameUs += totalUs;
        shell_print(sh, "ch%u [%s]: %d.%02d us per conversion (shift %d.%02d us -> SPI ~%d kHz for 19 bits);"
                    " values min %u mean %u max %u", ch, R5SPI_name(),
                    (int)totalUs, (int)(totalUs * 100) % 100, (int)shiftUs, (int)(shiftUs * 100) % 100,
                    (int)(WORD19_BITS * 1000.0 / shiftUs), (unsigned int)minValue,
                    (unsigned int)(sum / ok), (unsigned int)maxValue);
    }
    if (frameUs > 0.0) {
        shell_print(sh, "one frame (2 conversions) takes %d.%02d us -> at most ~%d frames/s with no margin",
                    (int)frameUs, (int)(frameUs * 100) % 100, (int)(1e6 / frameUs));
    }
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(r5capCommands,
    SHELL_CMD(start, NULL, "start [frames_per_s]: (re)start capture", cmdStart),
    SHELL_CMD(stop, NULL, "stop capture", cmdStop),
    SHELL_CMD(stats, NULL, "show the ring header", cmdStats),
    SHELL_CMD(spiclk, NULL, "spiclk <hz>: set the SPI clock (capture must be stopped)", cmdSpiclk),
    SHELL_CMD(spitest, NULL, "time ADC conversions, measure SPI clock (capture stopped)", cmdSpitest),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(r5cap, &r5capCommands, "r5capture sampling", NULL);
