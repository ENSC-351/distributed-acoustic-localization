# dsp: two-microphone TDOA and direction estimator

Status: **offline draft, tested on synthetic data only.** Not yet used by `node`.

Takes one aligned two-channel event window (from the detector/window extractor),
finds the delay between the microphones by cross-correlation or GCC-PHAT, and
returns a far-field bearing, or an explicit reason why it refused to.
API: [`include/dsp/tdoa.h`](include/dsp/tdoa.h).

## Conventions

| Quantity | Definition |
| --- | --- |
| `tdoa_s` | `t_B - t_A` (physical arrival times). Positive: sound reached **A** first. |
| `channel_skew_s` | `sampling_time_B - sampling_time_A` for the same frame index. |
| Correlation lag | `r[k] = sum_n a[n] * b[n + k]`. B delayed by D samples → peak at `k = +D`. |
| Skew correction | `tdoa_s = peak_lag / fs + channel_skew_s` |
| Geometry | Mic A at `x = -d/2`, B at `x = +d/2`. `theta = 0` broadside, positive toward B, forward half-plane only. |
| Angle | Far-field: `theta = asin(-c * tdoa / d)`, degrees in [-90, 90]. |

The delay-sign, channel-swap and skew tests in `tests/test_tdoa.c` pin these
down. If any convention changes, change the tests first.

**MCP3208 skew:** the ADC converts one channel at a time, so B is sampled
slightly after A (or before, depending on read order). Measure it and set
`channel_skew_s`; don't shift the samples upstream as well, or it is corrected twice.

## Results and statuses

`status == DSP_TDOA_OK` is the only case with a `tdoa_s` / `angle_deg` (they
are NaN otherwise). Refusals: bad arguments, non-finite samples, low energy, no
peak, strongest peak outside the physical range, delay outside range, weak peak.
Diagnostics (raw lag, peaks, peak-to-RMS, search range, RMS, skew) are filled in
as far as processing got. `confidence` is `1 - second_peak / main_peak`: a
heuristic, **not** a probability.

Accepted range: `|tdoa| <= d/c + bound_tolerance_samples / fs`. Inside that but
past `d/c`, the asin input is clamped and `DSP_TDOA_FLAG_CLAMPED` is set.

## Things that are placeholders

- **`min_peak_to_rms = 7`** was chosen from synthetic data: noise-only windows
  never exceeded 5.7 over 200 trials, correct estimates were mostly 15-45.
  Retune on real recordings. GCC-PHAT refuses many more noisy band-limited
  windows than plain xcorr at this threshold, so the two methods may need
  different values.
- **`phat_floor = 1e-3`**, `ambiguity_ratio = 0.8`: untested on real data.
- **Test geometry** (fs = 40 kHz, d = 0.30 m) is made up. Spacing is TBD and the
  per-channel sample rate hasn't been measured.

## Resolution: pick spacing with this in mind

Max delay spans `fs * d / c` samples each side of zero. Fewer samples means
coarser angles, especially near broadside where one sample is the biggest jump.

| per-channel fs | d = 0.10 m | d = 0.20 m | d = 0.30 m |
| --- | --- | --- | --- |
| 10 kHz | 2.9 | 5.8 | 8.7 |
| 20 kHz | 5.8 | 11.7 | 17.5 |
| 40 kHz | 11.7 | 23.3 | 35.0 |

(samples, c = 343 m/s). Sub-sample interpolation helps a bit but doesn't fix
noise, skew or aliasing.

## Building and running the tests (host)

Tests build only when not cross-compiling. From the project root:

```
cmake -S . -B build/host -G Ninja
cmake --build build/host --target dsp_tests
./build/host/dsp/tests/dsp_tests
```

Building only the `dsp_tests` target avoids building `node`/`master`, whose
post-build step copies them to `~/ensc351/public/myApps/`.

## Command-line tool

`tdoa_cli` runs the estimator on one window from a CSV file (one `a,b` pair
per line; lines starting with `#` are skipped) and prints JSON:

```
tdoa_cli --fs 48000 --spacing 0.10 --method xcorr --no-dc window.csv
```

Use `--no-dc` for windows from the detector, which already removes DC.
Other options: `--skew S`, `--c MPS`, `--no-interp`, `--min-psr X`.
Exit code 0 = valid estimate, 1 = refused, 2 = usage or file error.

## Not done yet

- Plugging into `node` (event window in, `proto_event` out).
- Timing on the BeagleY-AI. No memory is allocated per estimate, but the
  time-domain xcorr is O(n²) (about 1M multiply-adds for n = 1024).
- Real recordings, skew calibration, near-field effects.
