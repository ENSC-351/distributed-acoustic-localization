# Handoff: ENSC 351 acoustic localization, Milestone 2 night (2026-11-16)

Written by Claude Code on Madden's Windows PC for Claude Code running on the
BeagleY-AI. Read this whole file first. Facts below were checked on
2026-11-16 around 16:00 PT; re-check repo state with `git fetch` before relying on it.

## The project

Several BeagleY-AI **nodes**, each with two microphones on an MCP3208 ADC,
estimate the direction of an impact sound (e.g. a marble landing) and send it
over TCP to a **master** BeagleY-AI, which will triangulate the position and
feed a host-PC GUI. C11, CMake, pthreads.

Repo: `ENSC-351/distributed-acoustic-localization` (GitHub), default branch `main`.

## People

- **Madden** (the user): TDOA / direction estimator, architecture, integration.
- **Jack Hinderager** (GitHub `JackHinderager`, branch prefix `jah28/`): wire
  protocol and networking (node side done on a branch, master not started).
- **Tyler**: DSP partner. Owns event detection, aligned window extraction,
  signal conditioning, test recordings, and capture characterization
  (per-channel rate, MCP3208 channel skew) with whoever does acquisition.
  None of Tyler's code is in the repo yet.
- Other team members, who owns ADC capture, and the master: **unknown, ask Madden**.

## Repo state

| Piece | Where |
| --- | --- |
| CMake skeleton, `common/` protocol pack/unpack (`protocol.h`) | `main` (PR #1, merged) |
| Node networking: connect/HELLO, heartbeats, EVENT sending, `node/include/node/dspLink.h` interface, `node/src/fakeDsp.c` mock | `origin/jah28/feature/node-networking`, 3 commits, **no PR, not merged** |
| Master | `main` has only a hello-world `master/src/main.c` |
| `fake_master.py` (Python stand-in master, stdlib only) | Not in repo. Jack sent it separately: Madden should drop it in `context/` |
| Madden's DSP estimator (`dsp/` library, tests, `tdoa_cli`) | Pushed to `origin/dsp/tdoa-estimator` (1 commit on main, **no PR yet**) |
| Detection, capture, real master, GUI | Not started in the repo |

**Do not commit, push, or open PRs unless Madden asks.**

## Setup on the Beagle

```bash
git clone https://github.com/ENSC-351/distributed-acoustic-localization.git dal
cd dal
# this folder lives on branch madden/context-handoff; unpack it without touching the index:
git archive origin/madden/context-handoff context | tar -x
echo 'context/' >> .git/info/exclude
git switch dsp/tdoa-estimator       # pushed 2026-11-16 (commit a445d9c), based on main
```
(`context/dsp-tdoa-estimator.patch` is the same change as a patch; it's only a
fallback now.)

The patch was verified to apply cleanly on both `origin/main` and
`origin/jah28/feature/node-networking`, and the combination of Jack's node +
the dsp library compiled and linked with `-Wall -Wextra -Wpedantic -Werror` on
Ubuntu 22.04 (x86). It has **not** been built on ARM yet.

### Build gotchas
- `cmake --build` of everything copies `node`/`master` into
  `~/ensc351/public/myApps/` (post-build step) and can overwrite the ARM
  binaries there. Build single targets (`--target dsp_tests`) or use plain gcc.
- The dsp tests are only added to CMake when **not** cross-compiling, which is
  true for a native build on the Beagle.
- Plain gcc, no CMake needed:
  ```bash
  F="-std=c11 -O2 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -pthread"
  gcc $F -Idsp/include dsp/src/tdoa.c dsp/tests/test_tdoa.c -lm -o dsp_tests
  ```
- In a bash script, background jobs ignore SIGINT, so `kill -INT` won't stop
  `fake_master.py`; use plain `kill` (SIGTERM). Jack's node does handle SIGINT.

## Tonight's deadline: Milestone 2 report

Instructor's requirements (transcribed from a screenshot):
- 2-sentence project description.
- Significant accomplishments, ~0.5 to 2 pages. **Three** must have proof
  (screenshot, photo, text capture of the app running, directory listing of a
  cross-compiled library, YouTube link for video...). Each proof must be
  referenced and explained: what it shows and how it demonstrates the accomplishment.
- Roadblocks / unexpected challenges, < 0.5 page.
- Significant differences from the project proposal, < 0.5 page.
  **We don't have the proposal. Ask Madden for it.**
- "Must have good progress to earn these marks."

Proofs already captured on the Windows PC (in `context/proofs/`):
1. `proof_networking_fake_master.log` + `proof_networking_node1.log`: Jack's
   node (built from his branch, run in WSL Ubuntu x86, not the Beagle) x2
   connecting to `fake_master.py` on localhost: HELLO accepted, heartbeats
   every 1 s, mock EVENTs every 2.5 s, ~0.2-0.4 ms latency, clean BYE on Ctrl+C.
   Reproduce with `context/run_networking_demo.sh` (needs `fake_master.py`
   next to it and Jack's branch checked out).
2. `proof_dsp_tests.txt`: estimator tests on Windows x86, 760/760 passing.

## Tasks for the Beagle, in priority order

### 1. Run the DSP tests natively on the BeagleY-AI (proof)
Build as above, then capture: `uname -a`, `file dsp_tests` (should say ARM
aarch64), and the test output into `context/proofs/proof_dsp_tests_beagle.txt`.
Nice to have: time `dsp_tdoa_estimate()` for n = 1024 with both methods
(`CLOCK_MONOTONIC`, a few hundred runs, report mean/max). The time-domain xcorr
is O(n²), about 1M multiply-adds per window.

### 2. Integration demo: real estimator → Jack's networking → master (best proof)
On a local branch (e.g. `demo/milestone2`) from `origin/jah28/feature/node-networking`,
apply the patch, then add `node/src/syntheticDsp.c` implementing `dspLink.h`,
and take `fakeDsp.c` out of the build (both define the same functions, and
`node/CMakeLists.txt` globs `src/*.c`). Link `node` with `dsp`
(`target_link_libraries(node LINK_PRIVATE common dsp)`).

`syntheticDsp.c` behaviour:
- Every 2.5 s, take the next "true" angle from a list such as
  -60, -30, -10, 0, 10, 30, 60 degrees.
- Delay in samples = `round(-sin(theta) * d / c * fs)` (convention below), with
  placeholder fs = 40000, d = 0.30, c = 343. Label these as placeholders.
- Build a window (n = 1024) like `make_pair()` in `dsp/tests/test_tdoa.c`: a
  decaying noise burst on A, the same burst delayed on B, plus small noise.
- Run `dsp_tdoa_estimate()` (GCC-PHAT). Print one line to stdout: true angle,
  delay, estimated angle, status, confidence, estimate time.
- If status is OK, return a `proto_event` with `theta = angle_deg`, `phi = 0`,
  `confidence`, `dominant_freq_hz = 0` (not implemented yet),
  `flags = PROTO_EVENT_THETA_MIRROR`, `t_event_ns = Timing_getRealtimeNs()`
  (placeholder: the real one is the trigger sample's time), `event_id` counter.
  If not OK, don't send. Count and print it.
- `DspLink_isDspAlive()`: true if a counter moved since the last call.
  `getDropCount()` = 0, `getMicCount()` = 2.
- `DspLink_takeEvent()` must never block (networking polls it every 20 ms from
  its own thread). For the demo it's acceptable to generate + estimate inside
  `takeEvent` when due, as `fakeDsp.c` does, as long as the estimate takes a few
  ms. Say so in a comment; the real design is a DSP thread + SPSC ring.

Build natively (no CMake):
```bash
gcc $F -Icommon/include -Inode/include -Idsp/include \
    common/src/*.c $(ls node/src/*.c | grep -v fakeDsp.c) dsp/src/*.c -lm -o node-demo
```
Run `python3 context/fake_master.py` (on the Beagle with `127.0.0.1`, or on
Madden's laptop) and `./node-demo 1 <master IP>`. Capture both outputs into
`context/proofs/`: the master log should show the estimated thetas matching the
node's "true" angles. This proves estimator + ARM build + networking together.

### 2b. Run Tyler's pipeline on the board (offline, easy proof)
```bash
sudo apt install -y python3-numpy python3-pytest     # if missing
(cd context/tyler_dsp_v2 && python3 -m pytest -q)    # his 36 tests on ARM
gcc $F -Idsp/include dsp/src/tdoa.c dsp/tools/tdoa_cli.c -lm -o tdoa_cli
python3 -I context/integration/check_pipeline.py context/tyler_dsp_v2 ./tdoa_cli /tmp/pipeline
```
Should match `context/integration/pipeline_check_output.txt` (from Windows).

**Option for task 2:** instead of generating synthetic windows in C, have
`syntheticDsp.c` replay Tyler's extracted windows from
`context/integration/windows/*.csv` (header line starting with '#', then `a,b`
per line, 48 kHz, DC already removed → `remove_dc = false`, method xcorr,
spacing placeholder 0.10 m). That demos Tyler's detector output → Madden's
estimator → Jack's networking → master on the board.

### 3. Report draft
Needs from Madden: the proposal, what other teammates did (hardware, ADC
capture, microphones), and confirmation of who's on the team. Suggested
three proofs: (a) DSP tests on the Beagle, (b) the integration demo,
(c) the networking log or a cross-compiled `file`/`ls` listing.
Be honest: synthetic data only, no real microphones, detection not started,
master not started, Jack's branch unmerged.

Candidate roadblocks/findings worth reporting:
- Noise alone produced in-range correlation peaks: without a peak-to-RMS check
  the estimator returned confident angles for pure noise (3/16 trials).
  Fixed with a `min_peak_to_rms` = 7 threshold (noise max was 5.7 in 200 trials).
- GCC-PHAT was not better than plain xcorr on noisy band-limited synthetic
  impacts (3/100 vs 38/100 accepted at noise 0.3); to be decided on recordings.
- Microphone spacing is still TBD, and spacing × per-channel sample rate sets the
  angle resolution (see the table in `dsp/README.md`).
- MCP3208 samples channels sequentially, so a channel skew must be measured
  and corrected (estimator supports it; not measured yet).
- EVENT protocol has no status field; decided to send only valid estimates.

## Tyler's handoff and the first integration check (2026-11-16 evening)

Tyler sent `dsp_v2.zip` (copied to `context/tyler_dsp_v2/`): a Python detector and
window extractor (`detector.py`, 36 pytest tests), a CLI (`run_detector.py`), and a
synthetic test set (`testdata/*.wav|npz` + `manifest.json`: delays +7, 0, -7, +3,
+2.5, low SNR, no impact, two impacts; 48 kHz). Windows are 960 samples (5 ms pre,
15 ms post). The `out/` folder he mentions was **not** in the zip; `run_detector.py` regenerates it.

His conventions match ours: delay = t_B - t_A, nothing shifted, skew passed
through, DC removed **once by him** (median of pre-trigger samples), so run the
estimator with `remove_dc = false` / `--no-dc` on his `samples_a/b`. His
`event_time_ns` = time of the window's **first sample** (we'd told Jack "trigger
sample"; adopt Tyler's and tell Jack).

New tool: `dsp/tools/tdoa_cli.c` (built as `tdoa_cli`) reads a CSV window and prints JSON.
`context/integration/check_pipeline.py` runs Tyler's detector on every test file,
feeds each window to `tdoa_cli`, and compares with the manifest; output in
`pipeline_check_output.txt`, details in `results.json`.

Results:
- All 8 cases correct end to end: no-impact gave no window, two impacts gave two windows.
- Plain xcorr: within 0.07 samples on clean cases; the 2.5 fractional delay gave 2.57.
  Low SNR gave 7.85, status OK but flagged AMBIGUOUS (second/peak 0.98).
- GCC-PHAT was worse: off by 0.3 to 0.7 samples (+7 gave 6.40, +3 gave 3.67), and it
  refused low SNR. Cause: Tyler's synthetic impact is a narrowband 1500 Hz ringing tone, and
  PHAT gives the noise-only bins equal weight. More evidence for backlog item 5; prefer xcorr for now.
- Tonal impacts plus wide spacing give a wrong-cycle risk: at d = 0.30 m the in-range second peak
  is the next tone cycle (32 samples away, 0.58 of the main peak); at d = 0.10 m there is none.
  If real impacts ring at f, keep spacing below about c / (2f) (11 cm at 1500 Hz), or rely on the
  envelope. **Record a real marble impact to see its spectrum before fixing spacing.**
- PSR for the tonal xcorr was about 7.8, barely above `min_peak_to_rms = 7` (tuned on broadband
  noise bursts). That threshold must be retuned on real recordings.
- The 48 kHz test rate is much higher than the MCP3208 will give per channel; regenerate the
  test set at the measured rate once capture exists.

## DSP conventions (pinned by tests)

- `tdoa = t_B - t_A`; positive means A heard it first.
- `skew = sampling_time_B - sampling_time_A`; physical `tdoa = peak_lag/fs + skew`.
- Correlation `r[k] = sum a[n] b[n+k]`; B delayed by D samples gives peak at +D.
- Mic A at x = -d/2, B at +d/2. theta = 0 broadside, positive toward B, front
  half-plane, degrees in [-90, 90]: `theta = asin(-c * tdoa / d)`.
- Statuses: OK, ERR_ARGS, ERR_NONFINITE, LOW_ENERGY, NO_PEAK,
  PEAK_OUT_OF_RANGE, TDOA_OUT_OF_RANGE, WEAK_PEAK. Angle/tdoa are NaN unless OK.
- `confidence = 1 - second_peak/main_peak` in [0, 1]: heuristic, not a probability.

## Answers drafted for Jack's EVENT questions (not confirmed as sent)

theta in degrees [-90, 90] relative to the node's mic axis (his mock uses
0-360: mismatch to flag); phi unused, PHI_VALID never set; THETA_MIRROR always
set; `t_event_ns` = CLOCK_REALTIME of the trigger sample (needs capture to map
sample index → time); confidence as above; only valid estimates sent;
dominant_freq_hz cheap via the GCC-PHAT FFT but not implemented; DSP-alive via
a counter; one EVENT per detected impact; SPSC ring between DSP and networking threads.

## DSP improvement backlog (current code is an MVP)

The estimator core works and refuses bad answers, but has only seen synthetic,
whole-sample delays. Numbers refer to the work order below.

Soon:
1. CLI tool to run real recordings/windows (CSV or WAV) through the estimator. Everything below needs it.
2. Time-domain xcorr: compute only the ~71 allowed lags instead of all 2047 (~30x cheaper).
3. Fractional-delay tests (currently integer delays only); measure parabolic-interpolation bias.
4. Better sub-sample precision: FFT zero-padded / upsampled correlation instead of the 3-point parabola.

Accuracy:
5. GCC-PHAT did worse than xcorr in noise. Try PHAT-beta (partial whitening) and/or band-limiting to the impact's band.
6. Correlate only the first few ms after onset to cut table reflections.
7. Calibrate: measured MCP3208 channel skew, speed of sound vs temperature, confidence vs measured error.
8. Fill `dominant_freq_hz` (nearly free from the GCC-PHAT FFT) for cross-node event matching.

Design questions for the team (decide before hardware is fixed):
9. A third mic per node: MCP3208 has 8 channels; a triangle gives full 360° and removes front/back ambiguity (and gives the protocol's `phi` a use).
10. Near-field: if impacts are close relative to mic spacing, the far-field angle is wrong. Cross-node arrival-time
    (TDOA) fusion isn't viable: chrony is ~1 ms and sound moves ~34 cm/ms, so per-node bearings stay the design.
11. Steadier sampling: user-space SPI reads have jittery timing, and jitter is angle error. The BeagleY-AI's real-time cores
    (course "RealtimeProcessor" lectures) could do the sampling. Acquisition owner's area, but the biggest accuracy lever.
12. Real node structure: DSP thread + SPSC ring to the networking thread, replacing the mock.

## Work order to "done and ready"

Final demo/report dates are unknown; ask Madden.

**Phase 0: Tonight (Milestone 2).** Tasks 1-3 above.

**Phase A: Decisions and merging (this week).**
- Decide: 2 vs 3 mics per node (9), mic spacing, target per-channel sample rate, who owns ADC capture, who owns the master.
- Jack opens a PR for node networking; Madden opens a PR for `dsp/`. Merge both to `main`.
- Send Jack the EVENT answers (incl. the theta range mismatch); agree the window file format with Tyler.
- Exit: one `main` with networking + estimator, and decisions written down.

**Phase B: Real data offline (needs Tyler's recordings and detector).**
- CLI tool (1); Tyler's detector exports windows; record impacts at known angles plus noise-only.
- Measure angle error; tune `min_peak_to_rms`, choose xcorr vs GCC-PHAT (5), add onset gating (6), fractional work (3, 4), lag-restricted xcorr (2).
- Time the estimator on the Beagle.
- Exit: documented angle error and refusal rate on real recordings.

**Phase C: Live single node (needs ADC capture).**
- Continuous capture into a ring buffer (consider the real-time cores, 11); live detector; DSP thread; real `dspLink` (12).
- `t_event_ns` from the trigger sample index mapped to CLOCK_REALTIME; measure and set channel skew (7).
- Exit: repeated live impacts produce valid/refused results with timestamps, no capture overruns; latency measured.

**Phase D: Master and fusion.**
- Node config (IDs, positions, rotations); ignore unsynced nodes; associate events across nodes by time (+ `dominant_freq_hz`, 8).
- Intersect bearings (weighted least squares; handle mirror solutions if still 2 mics); check near-field (10).
- Exit: position estimates from >= 2 real nodes.

**Phase E: System test, GUI, final report.**
- Host-PC GUI; accuracy across the sensing area; false triggers; end-to-end latency; final write-up.

Critical path: ADC capture (C) and Tyler's detector (B) gate everything after Phase A. Mic count (9) gates hardware.

## Files Madden should add to this folder

- `fake_master.py` (from Jack)
- `mock-dsp-summary.md`, `mock-master-summary.md` (from Jack)
- `ENSC351_DSP_MVP_Roadmap.md`, `ENSC351_DSP_Partner_Message.md`
- The project proposal
- The Milestone 2 requirements screenshot (optional, transcribed above)
