# DSP prototype: event detection and signal preparation (v2)

Offline Python reference for one two-microphone node. It detects impacts in a
two-channel recording and extracts aligned windows plus metadata for the
TDOA / direction estimator. Field names follow the roadmap's `EventWindow` table.
Synthetic data only so far; no real microphone recording has been tested.

## Quick start (from the `dsp/` folder)
```bash
python3 -m pytest -v                  # 36 tests
python3 make_testset.py               # writes testdata/ (WAV + NPZ + manifest.json)
python3 run_detector.py testdata/delay_plus7.wav --out-prefix out/run1
python3 demo.py                       # summary + detection_plot.png + window_plot.png
python3 sweep.py                      # detection / false-trigger rate vs noise
```
Needs `python3 python3-numpy python3-matplotlib python3-pytest`.

## Files
| File | Purpose |
|---|---|
| `detector.py` | `detect_impact()`, `detect_events()`, `calibrate_noise()`, `EventWindow`; pure functions, no I/O |
| `io_utils.py` | load stereo WAV / `.npy` / `.npz`; write WAV; save an event as `.npz` + `.json` |
| `run_detector.py` | command line: recording in, windows + metadata out |
| `synth.py`, `make_testset.py` | synthetic known-delay data and the generated test set |
| `sweep.py` | detection and false-trigger rate vs SNR at two thresholds |
| `demo.py` | summary and plots |
| `tests/test_detector.py` | pytest suite |

## Conventions (agree with Madden; the roadmap proposes these)
- **Delay** = t_B - t_A. Positive means channel B (row 1) hears the impact **later** than A (row 0). Tested, including channel swap.
- **Sample rate** is the effective **per-channel** rate, not the ADC's aggregate rate.
- **DC removal for the hand-off happens once**, per window, from the pre-trigger samples. `samples_a/b` are DC-removed; `raw_a/b` are untouched. Madden should not remove DC again. (Detection has its own internal DC estimate that never touches the output.)
- **Nothing is shifted.** `channel_skew_s` and `mic_spacing_m` are passed through; `skew_correction_applied_s` is always 0.
- **`trigger_index` is within the window.** `record_trigger_index` is in the original record. The trigger is an energy crossing and can lag the true onset by up to `smooth_samples` (0.5 ms by default), more at low SNR. Do not use it as a timing reference.
- **`event_time_ns`** (optional) refers to the window's **first sample**: buffer start + `window_start / fs`. Use `with_buffer_start_time(event, buffer_start_ns)`. Empty offline.

## Interface
```python
from detector import detect_impact, detect_events, DetectorConfig
ev  = detect_impact(data, fs, mic_spacing_m=None, channel_skew_s=None)   # first event
evs = detect_events(data, fs, DetectorConfig(holdoff_ms=50))             # all events
```
`data` is an array `(2, N)` or a pair `(a, b)`. `EventWindow` fields: `valid`, `status`,
`samples_a`, `samples_b`, `raw_a`, `raw_b`, `sample_count`, `sample_rate_hz`,
`channel_order`, `trigger_index`, `record_trigger_index`, `window_start/end`,
`event_time_ns/clock`, `mic_spacing_m`, `channel_skew_s`, `skew_correction_applied_s`,
`flags` (`incomplete`, `clipped_a/b`; clipping is `None` unless `adc_range` is given),
`processing` (list of steps), `diagnostics` (noise floor, threshold, peak/floor,
holdoff, `max_delay_samples` once spacing is given).
Bad input returns `valid=False` with an explicit status, never a crash or a made-up window.

## How it works
1. Detection-only DC removal (median) and a 0.5 ms moving average of the summed two-channel power.
2. Trigger at `threshold_ratio` (default 12) times the noise-floor energy: the record median offline, or a fixed `NoiseModel` from `calibrate_noise(quiet_capture)` for live use.
3. Window = `pre_ms` (5) before and `post_ms` (15) after, cut at the **same indices** on both channels.
4. After a trigger: holdoff (50 ms), then re-arm once energy falls below half the threshold, so one impact triggers once.
5. Windows that would leave the record are returned as invalid (`incomplete`).

## Measured behaviour (synthetic, `sweep.py`, 100 trials per point)
At ratio 12, detection is 100% down to 20 dB peak SNR and 0% at 14 dB. At ratio 5 it stays 100% at 14 dB, 38% at 10.5 dB, and 0% at 6 dB. Noise-only false triggers were 0% in every case, but the noise is white Gaussian, so real recordings will need tuning. Trigger lag grows with noise (median 2 samples at 34 dB, 13 at 14 dB with ratio 5).

## Known limits and not done yet
- **No real recordings tested.** Thresholds are unvalidated against real impacts and room noise.
- **Capture not characterized:** per-channel sample rate, sequential-channel skew, dropped samples. The detector reports skew but does not measure it.
- **Mic spacing unknown.** Once measured, pass `mic_spacing_m`; `diagnostics["max_delay_samples"]` (fs * d / c, c = 343 m/s) shows how many distinguishable delays exist.
- **No filtering yet.** The roadmap says to choose a filter from real recordings, apply it identically to both channels, and report its effect on delay.
- **No onset refinement:** the trigger index lags the onset slightly.
- Python reference only; the C port and ring-buffer handling for live capture are not written. `detect_events` works on a finished record, not a stream.
