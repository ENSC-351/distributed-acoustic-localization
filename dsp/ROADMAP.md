# DSP roadmap: after the two-microphone MVP

Status as of 2026-11-26 (branch `madden/node-integration`). The MVP runs end to end:
R5 sampler → detector → TDOA estimator → node → master, live on Joey's board.
Real-data evidence so far: one clap recording and one live tap run
([results](../r5capture/docs/results/claps_joey_board_2026-11-26.md)). True angles have
**not** been measured yet.

## Where each roadmap item stands

| Item | Status | Done so far | Still to do |
|---|---|---|---|
| **Prerequisite: real sample rate and channel skew** | ✅ Done | R5 timer-paced frames at 20,000.0/s, 0 late frames. Skew 17.52 µs (firmware), 17.45 µs (Joey's independent check). Linux reads the R5 timer: precise event times | Check the timer's true frequency against chrony over minutes (`hal_r5_timer.c` hints 0.2 % fast; negligible for angles) |
| **Sub-sample TDOA** | ✅ Basic | Parabolic interpolation around the correlation peak (`interpolate = true`). Clean synthetic taps at 6.5 cm, 1 m: ±60° marks read ~2° short (0.0-0.6° elsewhere); near-field is only ~0.03° there, so this is interpolation bias (`analyze_marks.py`, 2026-11-28) | Measure its bias on synthetic fractional delays; try upsampled correlation (8-16×, FFT zero-padding or sinc). Near broadside 1 sample = 3.6° at 20 kHz and 0.27 m |
| **Confidence and rejection** | 🟡 Partly | Peak-to-RMS, second-peak ratio (`confidence`), ambiguity flag, explicit refusals with a reason; the node counts refusals. `min_peak_to_rms` retuned 7 → 3 on real claps | Check whether high confidence really means low error (needs true angles); combine metrics into one calibrated score |
| **Filtering and detection** | 🟡 Partly | 500 Hz high-pass; energy trigger vs. a noise floor calibrated at start-up; threshold 300×, holdoff 300 ms, window 2 + 5 ms. Clap recording: 9/9 claps, 0 false triggers | Band-pass; adaptive floor or STA/LTA; per-channel noise normalisation (ch0 is 10× noisier and dominates the trigger). Tyler's area |
| **Handle reflections** | 🟡 Partly | Window 20 → 7 ms removed most echo-caused refusals; a strongest peak outside the physical range is refused, not trusted | Favour the first arrival: start the window at the onset; cross-check with the onset-time difference between channels |
| **Multi-node fusion** | 🟡 Pieces exist | `madden/feature/web-gui`: master triangulation, fusion, dashboard. Nodes now send real bearings with precise times | Merge the GUI master with this branch; real node positions/orientations; associate events by `t_event_ns`; fill `dominant_freq_hz` (still 0) to help matching |
| **Advanced noise reduction** (spectral subtraction, Wiener) | ❌ Not started | | Only if recordings show it's needed. Fix hardware first (ch0 noise, clipping); filtering can't undo clipping |
| **Stretch: beamforming, ML classification** | ❌ Not started | | Low priority |

## Improvements, in order of payoff for effort

1. **Measure first: marked-position recording.** Tape at about 1 m from the pair's centre at −60, −30, 0, +30, +60° (negative = mic A side), 5 taps each, ~3 s pause between marks. Record raw with `node/board/record_claps.sh` and run the live node too. Every change below is judged on this data with `dsp/tools/pipeline_cli`.
2. **Balance the channels in the detector** (small). Scale each channel by its own noise std from calibration before summing energy, so a noisy mic doesn't dominate triggering.
3. **Finer delay resolution** (medium, biggest accuracy gain near broadside). Upsample the correlation around the peak; compare bias and spread against parabolic interpolation. The live front taps spread −6° to +6° (about ±2 samples).
4. **Direct path only** (medium). Window aligned to the detected onset, tapered (Hann). Second estimator: onset-time difference per channel (survives clipping, since onset precedes it). Agreement between the two → high confidence; disagreement → flag.
5. **Better frequency weighting** (medium). PHAT limited to the clap band (roughly 500 Hz-5 kHz in the recording), or ML/SCOT weighting using the noise spectrum from calibration. Plain PHAT did worse on real claps because it boosts noise-only bins. Optionally notch ch0's 7.2 kHz tone.
6. **Calibrate geometry from data** (small). Least-squares fit of effective spacing and skew from taps at known angles: corrects mic-centre error, speed of sound, unmodelled channel delay.
7. **Meaningful confidence + `dominant_freq_hz`** (small-medium). One score calibrated on the marked-position data; dominant frequency per event for cross-node matching.
8. **Adaptive noise floor** (small). Update slowly while idle, instead of once at start-up.

Later / larger:
- **Near-field correction.** The far-field formula's error at 0.27 m spacing is about 0.2° at 1 m, 0.9° at 0.5 m and 2.3-2.7° at 0.3 m (computed, exact geometry vs. formula). Only matters for impacts very close to a node. Option: send the raw TDOA and let the master triangulate with hyperbolas.
- **Front/back ambiguity.** Only a third microphone resolves it (`PROTO_EVENT_THETA_MIRROR` until then).
- **Sample rate / spacing.** 24k works when quiet; wider spacing helps resolution more than rate, at the cost of spatial aliasing (c / 2d = 635 Hz at 0.27 m).

## Hardware issues found (fix before blaming the DSP)

| Issue | Evidence | Effect |
|---|---|---|
| Claps clip in the mic amplifiers | Every clap stops at ~3340 (ch0) / ~3630 (ch1), far below 4095 | Distorted waveform, broad correlation; lower the gain or tap softer |
| ch0 is 10× noisier than ch1 | Silence std 22 vs 2.3 counts; 100-500 Hz and a 7.2 kHz tone | Dominates the trigger energy; lowers peak-to-RMS |

## How to evaluate any change

Compare the baseline and the change on the **same recordings**, offline:
```
python3 -c "import wave,numpy as np; w=wave.open('rec.wav'); np.savetxt('rec.csv', np.frombuffer(w.readframes(w.getnframes()),'<i2').reshape(-1,2), fmt='%d', delimiter=',')"
pipeline_cli rec.csv hp=500 threshold=300 holdoff_ms=300 pre_ms=2 post_ms=5 min_ptr=3 quiet_s=5
```

| Metric | Current value | Source |
|---|---|---|
| False detections | 0 (clap recording, after tuning) | `pipeline_cli` `quiet_s` + known clap times |
| Invalid / refused results | 27 % (live taps) | node summary |
| Missed impacts | unknown | needs ground truth |
| Angular error (mean, spread per angle) | unknown | needs marked positions |
| Processing time per event on the board | not measured | add timing to the node |

## Rules that must not change

- Neither channel is ever shifted. Channel skew is corrected in exactly one place: the
  estimator (`channel_skew_s`). Filters must be identical on both channels.
- Conventions in `dsp/README.md` (sign of TDOA, theta positive toward mic B) are pinned by
  `dsp/tests/test_tdoa.c`. Change the tests first if a convention changes.
- Ownership: the estimator (`dsp/src/tdoa.c`) belongs to the DSP teammate who wrote it;
  the detector follows Tyler's algorithm. Agree changes with them.

## Tools

| Tool | Use |
|---|---|
| `dsp/tools/pipeline_cli` | Detector + estimator over a recording with any settings |
| `dsp/tools/detector_cli`, `compare_detector.py` | Detector alone; check against Tyler's `detector.py` |
| `dsp/tools/make_replay.py` | Synthetic bursts with known delays |
| `node/board/record_claps.sh` | Guided raw recording on the board |
| `node/board/node_r5_test.sh` | Live node + master on the board (`replay`, `silence`, `taps`) |
| Node `DSP_*` variables | Change any setting live without rebuilding (`node/README.md`) |

## Node calibration (per node, ~5 min; do before triangle accuracy tests)

Measured from a marked-position recording (`node/board/record_marks.sh`, **soft claps** so
nothing clips; −90 on the mic A / left side looking out from the node), analysed with
`dsp/tools/fit_marks.py`, which fits `lag = slope·sin(mark) + intercept`:

| Parameter | From | Setting | Madden's node (2026-11-28, claps) |
|---|---|---|---|
| Effective spacing | `|slope|·c/fs` | `DSP_MIC_SPACING_M` | 4.6 cm (ruler: 6.5 cm) |
| Timing offset beyond the R5 skew | `intercept + skew` | needs a `DSP_SKEW_OFFSET_US` option (not written yet) | −11 µs |
| Orientation | slope sign / scratch test | `DSP_SWAP_MICS` | correct (A = ch0 = left) |
| Mounting angle in the triangle | clap at the centre: each node should read 0° | per-node heading in `master/src/layout.c` | not measured |
| Position | tape measure | `LAYOUT_SIDE_M`, `layout.c` | not measured |

Always confirm on a **second, fresh** recording: fitting and scoring on the same data is optimistic.

Why accuracy is what it is: `Δθ ≈ c·Δτ / (d·cos θ)`. Madden's claps: Δτ ≈ 0.43 samples
(21 µs), d = 4.6 cm → ~9° at broadside, close to the measured 11.6° mean. Levers: smaller Δτ
(cleaner signal, better DSP, below) or larger d (wider spacing: 10 cm ≈ 2× better, 27 cm ≈ 6×).

## Node-side DSP improvements: nice to have, after the triangle tests

Scored offline on recordings with known truth before any default changes; each change behind
a `DSP_*` setting. Baseline: claps on Madden's node, 11.6° mean |error| at 4.6 cm.

**0. Benchmark first.** One command that runs the node DSP over every recording with truth
(clap marks, tap marks, synthetic marks, Tyler's files) and prints mean/RMS error, accepted %,
false triggers. Extend `dsp/tools/analyze_marks.py`.

**1. Signal conditioning**
- Band-pass (≈500 Hz-5 kHz) instead of the 500 Hz high-pass: keeps the timing band, drops
  ch0's 7.2 kHz tone and broadband noise.
- Clipping handling: correlate only up to the first clipped sample, or drop clipped samples
  (every clap clipped at ~3300 counts).
- Per-channel normalisation by each channel's calibrated noise level (ch0 is 10× noisier on
  Joey's board).
- Optional notch for a known tone (board-specific).

**2. Detection**
- Per-channel normalised trigger energy.
- Precise onset per window (energy rise / threshold crossing, ~0.1 ms), instead of a fixed
  2 ms before the trigger point.
- Adaptive noise floor while idle.

**3. Delay estimation (main lever)**
- a. Upsampled / sinc-interpolated correlation peak: removes the parabola bias (~2° at ±60°
  on clean data). `fit_marks.py` already does this offline.
- b. Direct-path window: onset → +1.5-2 ms, Hann taper; fewer reflections in the estimate.
- c. Second estimator: onset-time difference between channels (survives echoes and clipping);
  agree → accept, disagree → flag/refuse.
- d. Band-limited GCC-PHAT or SCOT/ML weighting using the calibrated noise spectrum.
- e. Search only the physical lag range; a stronger peak outside it becomes a warning, not a
  refusal (16 of 24 taps refused this way on Madden's board, often with plausible in-range lags).
- f. Calibrated timing offset (`DSP_SKEW_OFFSET_US`).

**4. Confidence and output**
- A confidence that works at 6.5 cm (it is ~1.00 on every tap now): peak sharpness,
  agreement of 3a/3c, clipping and noise flags.
- Fill `dominant_freq_hz` for cross-node matching.

Suggested order: 0 → 3a → 3b (with 2's onset) → 1 (clipping, band-pass) → 3e → 3c → 4.
Ownership: 3a/3d/3e touch `dsp/tdoa.c` (agree with its author, or add as non-default
options); detector changes past Tyler.
