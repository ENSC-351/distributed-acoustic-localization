# Handoff: the master's localizer, and connecting the nodes to the master

For Zur (master networking / EventBuffer), from Madden. Written 2026-11-28.

**Goal:** real node events → `netServer` → your `EventBuffer` → **localizer** → fusion
(triangulation) → web GUI. Every piece exists except the localizer, and the pieces live on
different branches. This file says what's where, what the localizer has to do, and what's
left before it all goes to `main`.

## 1. What's where

| Piece | Branch | Who | State |
|---|---|---|---|
| Node: R5 sampling, detector, TDOA estimator, real `dspLink.c` | `madden/node-integration` (**local only, not pushed yet**) | Madden, Joey, Tyler's algorithm, estimator from `dsp/tdoa-estimator` | ✅ Works live on Joey's board; real taps reached the master |
| Node networking (`netClient.c`) and master `netServer.c` | `main` | Jack | ✅ Works; unchanged |
| `EventBuffer` (ring of received events) | `zta32/feature/master-networking` | Zur | 🔴 **Doesn't build** (see 2) |
| `Localizer` | `zta32/feature/master-networking` (`fakeLocalizer.c`) | **Zur, or Madden; decide** | ❌ TODO skeleton |
| Fusion, Locate, Layout, Dashboard, UDP, web GUI | `madden/feature/web-gui` | Madden | ✅ Written; so far only fed by `master --demo` |

## 2. Fix first: conflict markers in `master/src/eventBuffer.c`

The merge from `main` (`f00de3c`) committed unresolved markers: `<<<<<<< HEAD` (lines 2, 77,
100), `=======` (58, 89, 182), `>>>>>>> 64db223…` (72, 94, 189). The compiler stops with
"version control conflict marker in file".

Fix: keep the **HEAD** side of each block (the real ring buffer) and delete `main`'s
placeholder side and the marker lines, then build and commit:
```
cmake -S . -B build && cmake --build build --target master
```
While you're there (optional): the timed wait in `EventBuffer_waitAndTake()` uses
`CLOCK_REALTIME`; chrony can step it, so `CLOCK_MONOTONIC` is safer (your own comment says so).

## 3. The localizer

### Where it sits
```
netServer thread ──EventBuffer_add()──▶ [EventBuffer] ──EventBuffer_waitAndTake()──▶ localizer thread ──Fusion_addEvent()──▶ fusion → dashboard → UDP → web GUI
```
The localizer is deliberately thin: fusion (`madden/feature/web-gui`, `master/src/fusion.c`)
already groups reports and intersects the lines. The localizer only moves events from your
buffer into fusion.

### Interface (keep `master/include/master/localizer.h` as it is)
```c
void Localizer_init(void);     // start the thread
void Localizer_cleanup(void);  // stop and join it
```

### What the thread does
```
while running:
    if EventBuffer_waitAndTake(&entry, 100 ms):        // timeout so it can notice shutdown
        Fusion_addEvent(entry.nodeId,
                        entry.event.theta,
                        entry.event.confidence,
                        entry.event.flags)
        (optional) print node, event_id, theta, confidence, latency
```
- `Fusion_addEvent()` is thread-safe (it has its own mutex) and ignores node IDs that aren't
  in the layout (it still counts them in the GUI's node table). No extra locking is needed.
- **Latency**, if printed: `recvTimeNs - event.t_send_ns`. Both are `uint64_t`, so compute it
  as `int64_t` (cast each side first): if the node's clock is slightly ahead of the master's,
  the unsigned difference wraps to a huge number. Don't print or use negative values as errors.
- Model for the thread's start/stop: `node/src/netClient.c` (an `isRunning` flag, a mutex or
  atomic, `pthread_create`, `pthread_join`).
- File name: replace `fakeLocalizer.c` with `localizer.c` (or keep the fake behind a CMake
  option, like the node's `NODE_FAKE_DSP`).

### What's in each event (real nodes, as of today)
| Field | Meaning |
|---|---|
| `theta` | −90 to +90°, 0 = straight out of the mic pair, positive toward mic B |
| `confidence` | 0 to 1, heuristic (not a probability). Fusion uses it as the line's weight. At 6.5 cm mic spacing it is almost always 1.00 (known issue, see `dsp/ROADMAP.md`) |
| `flags` | `PROTO_EVENT_THETA_MIRROR` always (2 mics); `PROTO_EVENT_CLIPPING` if the audio clipped |
| `event_id` | Counts every window the node's detector found, **including ones it refused to send**. A gap is normal and does **not** mean network loss |
| `t_event_ns` | `CLOCK_REALTIME` of the first sample of the analysed window, from the R5's timer (µs-accurate on the node). Comparable across nodes only as far as chrony syncs them (heartbeat: `clock_synced`, `clock_err_us`) |
| `dominant_freq_hz` | 0 for now |

Fusion currently groups reports by the **master's receive time** (150 ms window), so it does
not depend on the node clocks.

## 4. The master's `main.c` (needs merging)

`main` / your branch: `Shutdown_init` → `EventBuffer_init` → `NetServer_init`, then wait.
`web-gui`: its own `Stop` module and `sigaction`, `Dashboard_init`, `Fusion_init`, `UDP_init`,
optional `--demo`.

Proposed single `main.c` (start in this order, stop in reverse):
```
Shutdown_init()          // first: blocks SIGINT/SIGTERM for every later thread
EventBuffer_init()
Dashboard_init(isDemo) + node poses from Layout
Fusion_init()
Localizer_init()         // takes from EventBuffer, gives to Fusion
NetServer_init(port)     // starts adding events
UDP_init()               // web GUI; its "stop" command calls Shutdown_request()
if --demo: DemoSource_init()
Shutdown_waitForRequest()
... cleanup in reverse ...
```
Usage would become `master [port] [--demo]`. Madden can do this merge; say if you'd rather.

## 5. Node placement (for the real triangle)

From `master/include/layout.h` (web-gui branch):
- Node **1** at the top, **2** bottom-left, **3** bottom-right; each faces the centre.
- Mount each node with **mic B on its right** when facing the centre (otherwise its line is mirrored).
- `LAYOUT_SIDE_M` = 2.0 m: measure the real triangle and update it.
- Each node runs `sudo DSP_MIC_SPACING_M=<its spacing> ./node <id> <master IP>` (Joey's board: 0.065 m).

## 6. How far we are from pushing

**Already on GitHub:** `main` (Jack's networking), `dsp/tdoa-estimator`,
`jkolsteren/feature/r5-capture` (R5 sampler), `zta32/feature/master-networking` (broken build),
`madden/feature/web-gui`.

**Local only** (`madden/node-integration`, on Madden's VM; merges `main` + `dsp/tdoa-estimator` +
Joey's R5 branch): streaming detector, R5 backend for `AudioCapture` with the clock mapping,
real `dspLink.c`, high-pass and tuned settings, test and board scripts, `dsp/ROADMAP.md`. Host
and ARM builds pass; unit tests pass (detector 8,880 checks, R5 backend 52, estimator).

### Checklist to get everything into `main`
| # | Step | Who | Status |
|---|---|---|---|
| 1 | Fix the conflict markers in `eventBuffer.c` and push | Madden (Zur agreed) | ✅ fixed on local branch `zta32/fix-eventbuffer-markers` (`eeafa31`); builds, buffer receives events. ☐ push |
| 2 | Decide who writes the localizer | Zur + Madden | ☐ |
| 3 | Push `madden/node-integration`; Jack reviews `node/src/dspLink.c` | Madden, Jack | ☐ (ready to push) |
| 4 | Merge EventBuffer + web-gui into the integration branch; one `main.c` | Madden | ☐ |
| 5 | Write `localizer.c` | Zur or Madden | ☐ |
| 6 | VM test: 3 replay nodes (IDs 1-3) with audio for a sound at a chosen (x, y) → master → GUI dot at that spot | Madden | 🟡 tool done (`dsp/tools/run_triangle_sim.sh`); nodes → master verified. Re-run once the localizer + fusion are in, and check the GUI dot |
| 7 | One real node + master on separate machines | Madden / Joey | ☐ |
| 8 | PRs to `main` in order: DSP estimator → R5/audio → node integration → EventBuffer → master + GUI + localizer | Each owner | ☐ |
| 9 | Three real nodes, taps at marked spots; measure position error | Team | ☐ |

Critical path: 1 → 4 → 5 → 6. Steps 1 and 2 are the only ones waiting on a decision.

## 7. Testing the localizer on its own

- `master --demo` feeds fake sounds through fusion (no networking): checks fusion and the GUI.
- `node` built with `-DNODE_FAKE_DSP=ON` sends a made-up event every 2.5 s: checks
  netServer → EventBuffer → localizer without any audio.
- Real DSP without hardware: `DSP_REPLAY=replay.csv ./node 1 127.0.0.1` with a file from
  `dsp/tools/make_replay.py` (`node/README.md`).
- **Three nodes, sounds at known positions** (the test for the localizer):
  ```
  dsp/tools/run_triangle_sim.sh <build dir> [spacing m] ["x,y x,y ..."]
  ```
  Generates each node's recording for sounds at the given map positions (same layout as
  `layout.c`: side 2.0 m, node 1 top, mic B on the right), starts a master and nodes 1-3
  together on localhost, and prints each node's theta error and the position from
  intersecting the bearings. Needs numpy. Result on 2026-11-28 (synthetic, current master,
  intersection done by the script): theta errors within about 1°, position error mean 1.5 cm
  at 0.065 m spacing, 0.6 cm at 0.27 m. With your localizer + fusion running, the master
  should locate the same positions (and the GUI show them).
