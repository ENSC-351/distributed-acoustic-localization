# Integration plan: R5 sampling → detector → DSP → node → master

Written 2026-11-25. Starting point: `jkolsteren/feature/r5-capture` at `64fda71`
(R5 samples both mics at 20,000/s, no late frames under load), `dsp/tdoa-estimator`
(C estimator, synthetic tests only), `main` (node/master networking with `fakeDsp.c`).
**Everything on the DSP side has only seen synthetic data so far.**

## Status (2026-11-25, branch `madden/node-integration`)

| Phase | State | Evidence |
|---|---|---|
| A: one branch | Done | Host + aarch64 builds with `-Werror`; all unit tests pass |
| B: R5 backend in `AudioCapture` | Done on the VM, **not run on a board** | `audio/tests`: fake ring + timer, 52 checks |
| C: R5 ticks to `CLOCK_REALTIME` | Done on the VM, **not run on a board** | Median 0.7 µs vs a fake timer, incl. a 0.2 % fast one and the 32-bit wrap |
| D: streaming detector in C | Done | Same triggers, window starts and DC as Tyler's `detector.py` on his 8 recordings at 4 block sizes; 8,874 unit checks |
| E: real `DspLink` | Done | `node/src/dspLink.c`; settings via `DSP_*` variables (`node/README.md`) |
| F1: replay | Done on the VM | node → master on localhost: 0, ±14.9, ±31.0, ±50.5° exactly as generated; synthetic silence: 0 events |
| F2–F5 | Not started | Board script: `node/board/node_r5_test.sh` (`replay`, `silence`, `taps`) |

All results so far are on **synthetic data**.

## Target data flow on a node

```
R5 firmware ──ring @0xA1F00000──▶ AudioCapture (R5 backend)
                                     │ blocks of 128 frames, tick timestamps → CLOCK_REALTIME
                                     ▼
                                  Detector (streaming C port of Tyler's detector.py)
                                     │ aligned two-channel window (5 ms pre + 15 ms post)
                                     ▼
                                  dsp_tdoa_estimate()  →  angle, confidence, status
                                     │
                                     ▼
                                  event queue ──DspLink_takeEvent()──▶ netClient ──▶ master
```

One DSP thread does capture → detect → estimate → enqueue. The networking thread
only pops the queue (`DspLink_takeEvent` must never block). Nothing in `netClient.c`
or `master/` needs to change.

## Phase A: one branch with all the pieces

| Step | Work | Done when |
|---|---|---|
| A1 | New branch `madden/node-integration` from `origin/main`; merge `dsp/tdoa-estimator`, then `jkolsteren/feature/r5-capture` | Merged; only `CMakeLists.txt` and `README.md` conflict (checked with a trial merge) |
| A2 | Root `CMakeLists.txt`: `common hal audio dsp node master audio_meter` | Host build and aarch64 cross build pass with `-Werror` |
| A3 | Run every existing test: `dsp/tests`, `r5capture/test`, Tyler's Python tests | All pass, results saved under `docs/results/` |

## Phase B: R5 backend for `AudioCapture`

Same public API (`audio/include/audio/audio_capture.h`), so the meter, recorder
and DSP don't care which sampler runs.

- **Selection:** add `backend` to `AudioCapture_Config`: `AUTO` (default), `R5`, `LINUX`.
  `AUTO` uses the R5 if the ring has the right magic and `state == RUNNING`, else the
  Linux sampler. **Never start the Linux sampler while the R5 is running** (both
  would drive MCU_SPI0).
- **Readers:** each `AudioCapture_Reader` wraps its own `R5CapReader`. The ring already
  supports independent readers, so no extra thread or copy is needed. `readBlock` with
  a timeout polls with a short sleep (a block is 6.4 ms at 20 kHz; sleep ~1 ms).
- **Field mapping:**

  | `AudioCapture_BlockInfo` | From the ring |
  |---|---|
  | `blockNumber`, `blocksSkipped` | `R5CapBlockInfo.blockNumber`, `.skipped` |
  | `framePeriodNs` | `framePeriodTicks / tickHz` (exact, timer-driven) |
  | `tFirstNs` | `firstTick` → `CLOCK_REALTIME` (Phase C) |
  | `gapBeforeNs` | from ticks, exact: 0 unless blocks were skipped |
  | `timingDisturbed` | `flags & (LATE_TICK | ADC_ERROR)` |
  | `getChannelOffsetNs(k)` | `channelOffsetTicks[k] / tickHz` (≈ 17.5 µs on Joey's build) |

- **Starting the R5:** the backend does not load firmware. It expects `r5_start.sh`
  (later a systemd unit, Phase 2 below). `/dev/mem` needs root; the node already runs
  under sudo.
- **Tests:** a fake ring in ordinary memory (the existing `r5capture/test` style):
  skipped blocks, a late block, wrap of the block counter, R5 stopping mid-read.

Done when `audio_meter` runs unchanged on the R5 backend on the board, and the
fake-ring unit tests pass.

## Phase C: R5 ticks → `CLOCK_REALTIME`

The R5 timestamps blocks in MCU_TIMER0 ticks (25 MHz, 32-bit, extended to 64 bits by
the firmware). Events need `CLOCK_REALTIME` so the master can match events across nodes.

- **First version:** Linux maps MCU_TIMER0 read-only (`0x04800000`, counter TCRR at
  `+0x3C`) and takes pairs `realtime₁, TCRR, realtime₂`, keeping the pair with the
  smallest `realtime₂ − realtime₁`. Extend TCRR to 64 bits using the high bits of the
  latest block's `firstTick`. Re-pair once per second and fit offset + rate over the
  last ~10 pairs (tracks drift and chrony adjustments).
- **Verify on the board first:** that the A53 can read that address at all, and the real
  tick rate. `hal_r5_timer.c` notes 25,050 counts per 1 ms busy-wait; if the timer really
  runs 0.2 % fast, timestamps drift 2 ms per second without the rate fit, and the true
  per-mic rate is 0.2 % off (negligible for angles).
- **Fallback** if the A53 can't read the timer: the R5 writes `(tick)` to the control page
  on request and Linux pairs it with its own clock (precision tens of µs).
- **Precision needed:** angles come from inside one window, so they don't depend on this.
  Only event matching at the master does, and milliseconds are enough for that at first.

## Phase D: streaming detector in C

Tyler's `detector.py` works on a whole recording. Live use needs a streaming version
with the same rules (trigger on both channels combined, cut the same indices from both,
never shift a channel, DC from the pre-trigger samples only).

- **Where:** `dsp/include/dsp/detector.h`, `dsp/src/detector.c`, no allocation after create.
- **State machine:** `IDLE` → energy > `threshold_ratio × floor` → `COLLECTING` (until
  `post_ms` of samples after the trigger) → emit window → `HOLDOFF` (`holdoff_ms`) →
  re-arm when energy < `rearm_ratio × threshold`.
- **History:** a sample ring of at least `pre_ms + post_ms` (400 frames at 20 kHz; use
  1024), so windows can span block boundaries.
- **Noise floor:** calibrate from ~1 s at startup (Tyler's `NoiseModel`), as in his live-use note.
- **Invalid windows:** discard if any block inside it was skipped or `timingDisturbed`
  (TDOA assumes even spacing). Set `clipped` if any raw sample is 0 or 4095.
- **Output:** float `a[]`, `b[]` (DC-removed), raw samples, `tFirstNs`, trigger index,
  clipped flag: everything `dsp_tdoa_input` needs.
- **Tests:** port Tyler's `test_detector.py` cases; golden-file check against the Python
  output (same trigger index and window bounds) on the synthetic records in
  `context/integration/`. Feed the same record in different block sizes; the windows
  must not change.
- **Owner:** Tyler wrote the Python; ask him whether he ports it or reviews our port.

## Phase E: the real `DspLink`

- `node/src/dspLink.c` replaces `fakeDsp.c`; CMake option `NODE_FAKE_DSP=ON` keeps the
  fake for networking tests without mics.
- **DSP thread:** `AudioCapture_readBlock` → detector → `dsp_tdoa_estimate` → queue.
  The ring buffers ~1.5 s, and an estimate takes ~7 ms on the board, so it keeps up easily.
- **Queue:** 16 events, mutex-protected; when full, drop the oldest and count it
  (`DspLink_getDropCount`). `DspLink_isDspAlive` = block counter moved.
- **Event mapping:**

  | `proto_event` | From |
  |---|---|
  | `t_event_ns` | window's first sample, `CLOCK_REALTIME` (Phase C) |
  | `theta` | `angle_deg` |
  | `confidence` | `confidence` |
  | `dominant_freq_hz` | 0 at first ("unknown" is allowed); later the FFT peak of the window |
  | `flags` | `PROTO_EVENT_THETA_MIRROR` always (2 mics) |
  | `event_id` | counter of detections, including refused ones, so the master sees gaps |

- **Refusals** (`status != OK`): not sent; counted per status and logged once per second.
- **Config** (args or a small file): mic spacing, which ADC channel is mic A,
  `channel_skew_s = offset(B) − offset(A)` from `AudioCapture_getChannelOffsetNs`.
  **Check the sign physically:** a tap on mic B's side must give positive theta.

## Phase F: end-to-end tests, in this order

| Test | Setup | Pass |
|---|---|---|
| F1 Replay | VM: synthetic two-channel records through detector + estimator (no hardware) | Same angles as the Python pipeline (within 0.3° on clean cases) |
| F2 Silence | Board, R5 running, room quiet, node → master, 10 min | Heartbeats only; count false events |
| F3 One mic on both inputs | Tap near the mic | TDOA ≈ 0 after skew correction (tests the skew sign and value) |
| F4 Two mics, known angles | Taps at −60, −30, 0, 30, 60° at ~1 m, 10 each; **also save every raw window** | Report mean and spread of error per angle, honestly |
| F5 Two nodes | Both nodes → master → web GUI | Triangulated point near the real tap |

Blockers before F4: the second mic drifting to 0 counts (Joey's notes), and clipping on
loud sounds (lower gain or tap further away).

## Afterwards: improvement order

In order of expected payoff. Each needs F4 recordings first, so changes are measured,
not guessed.

1. **Tune on real data.** Use the saved F4 windows offline: `min_peak_to_rms` (7 is from
   synthetic data), detector `threshold_ratio`, xcorr vs GCC-PHAT, shorter post-trigger
   window to cut reflections. Done when the false-event rate (F2) and angle error (F4)
   are both reported for the chosen settings.
2. **Geometry and filtering.** At 20 kHz and 10 cm spacing the full delay range is only
   ±5.8 samples; 20–30 cm gives 12–17 (finer angles near broadside) at the cost of
   spatial aliasing above ~600–850 Hz. Decide with the team from a real tap's spectrum.
   An analog anti-aliasing filter (nothing above 10 kHz) if the hardware allows.
3. **Robustness.** Adaptive noise floor while idle; recover if the R5 stops (state not
   `RUNNING` → warn, keep heartbeats, reattach); start the R5 at boot (systemd unit
   running `r5_start.sh`); reject clipped windows or lower their confidence.
4. **Timing.** Measure the true R5 tick rate against chrony time over minutes; refine the
   clock fit; report event-time error between two nodes for the same tap.
5. **Speed, only if needed.** Limit the correlation to the physical lag range (±a few
   dozen lags instead of the whole window) if it isn't already; measure
   `t_send − t_event` latency. Events are rare, so this matters least.
6. **Sample rate.** 24k works quiet, 20k is the default with margin under load. Raising
   the rate helps less than wider spacing; ADC ceiling ~30k per mic at 3.3 V.
7. **Out of scope unless time:** third mic (360°, `phi`), R5 doing detection itself.

## Questions for the team

- Mic spacing (decides Phase E config and improvement 2).
- Tyler: port the detector, or review a port?
- Joey: is the second mic's circuit fixed?
- Merge order to `main`: the DSP branch first (no hardware), then the R5/audio branch, then integration.
