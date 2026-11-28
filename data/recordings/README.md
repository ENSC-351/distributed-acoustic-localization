# Real microphone recordings (for detector work)

For Tyler, from Madden. The first real two-mic recordings, taken on the node hardware with
the R5 sampler. Use them to test and tune `detector.py` (and the C port in
`dsp/src/detector.c`) on real impacts instead of synthetic ones.

## Format

Same as your `testdata/*.npz`, so `io_utils.load_recording()` and `detect_events()` read
them as they are:

| Key | Contents |
|---|---|
| `data` | `int16`, shape `(2, N)`: raw 12-bit ADC counts (0-4095, idle ≈ 2000). Row 0 = mic A (ADC ch0), row 1 = mic B (ch1) |
| `fs` | 20000.0 (per channel; timer-paced by the R5, exact) |
| `channel_skew_s` | 17.52e-6: mic B is sampled 17.52 µs after mic A in every frame. Pass it through; the estimator corrects it |
| `mark_deg`, `prompt_time_s` | Marked-angle recordings only: the **true** angle of each impact and the time (s into the recording) the person was told to make it. The impact follows within ~0.1-0.5 s |

Sign convention (as in `dsp/README.md`): theta 0 = straight out of the mic pair, negative on
the mic A side (the left, looking out from the node), positive on the mic B side.

```python
import numpy as np
r = np.load("data/recordings/claps_marks_madden_2026-11-28.npz")
data, fs = r["data"].astype(float), float(r["fs"])
```

Sampling was clean in every file: 0 blocks skipped, 0 late frames, 0 ADC errors.

## The recordings

| File | Length | Board / spacing | What happens | Ground truth |
|---|---|---|---|---|
| `claps_joey_2026-11-26.npz` | 44 s | Joey's, 0.27 m | 5 s silence, then 9 claps 3 s apart: 3 on the mic A side, 3 on the mic B side, 3 "in front" | Sides only (positions not marked) |
| `taps_marks_madden_2026-11-28.npz` | 103 s | Madden's, 0.065 m | 3 s silence, then 5 table taps at each mark: −90, −60, −30, 0, +30, +60, +90°, at ~1 m | `mark_deg`, `prompt_time_s` |
| `claps_marks_madden_2026-11-28.npz` | 103 s | Madden's, 0.065 m | Same, but claps in the air above each mark | `mark_deg`, `prompt_time_s` |
| `scratch_ch0_madden_2026-11-28.npz` | 8 s | Madden's | Left mic (mic A) scratched with a fingernail at 3-5 s | Identifies ch0 |

In the original files the tape for the Madden recordings was laid out mirrored (−90 on the
mic B side). `mark_deg` here is already corrected to the true angles.

## What we found in them (things your detector has to cope with)

| | Joey's board | Madden's board |
|---|---|---|
| Noise in silence (std, counts) | ch0 22, ch1 2.3: ch0 is 10× noisier | ch0 ~15-18, ch1 ~14-15 |
| Noise spectrum | ch0: 47 % at 100-500 Hz, a 7.2 kHz tone; ch1: mostly < 100 Hz | |
| Impact peaks | Every clap **clips** at ~3340 (ch0) / ~3630 (ch1), below the ADC's 4095: the mic amplifiers saturate | Claps and taps clip at ~3300 on both channels |
| DC | Steady all recording (no drift) | Steady |

- **Table taps vs claps:** taps on the table the node sits on reach the mics partly through
  the table, which distorts the delay between channels (effective spacing 2.7 cm, and not a
  sine of the angle). Claps in the air behave properly (4.6 cm, a clean sine). For the
  detector both are fine; for the angle, prefer claps or isolate the node.
- **Impacts are huge compared to the noise floor:** the combined energy reaches 6,500-11,000×
  the median noise energy, while background noise (movement, room) reaches up to ~200×.

## Detector results on these files

`detect_events()` with your default config (threshold 12×, holdoff 50 ms), no high-pass:

| File | Events | Real impacts |
|---|---|---|
| `claps_joey_2026-11-26` | 130 | 9 |
| `claps_marks_madden_2026-11-28` | 117 | 35 |

With `threshold_ratio=300, holdoff_ms=300`: exactly the 9 claps (at 9.45, 12.34, 15.28,
21.48, 24.42, 27.42, 33.66, 36.46, 39.56 s) and 34 of the 35 marked claps.

The node currently runs the C port with: 500 Hz high-pass before detection, threshold 300×,
holdoff 300 ms, window 2 ms before + 5 ms after the trigger (shorter windows had fewer
reflections in them). These were tuned on `claps_joey` only; the details are in
`r5capture/docs/results/claps_joey_board_2026-11-26.md`.

## Ideas worth trying (your call)

- A threshold that adapts to each channel's noise (ch0 on Joey's board dominates the energy).
- A precise onset per event (to ~0.1 ms) instead of the trigger point, so the window can stop
  before reflections arrive (1-3 ms after the direct sound).
- Flag or trim clipped samples in the window.
- A slowly adapting noise floor while idle.
- Missed / false detections against `prompt_time_s` on the marked files: an event within
  ~1.5 s after a prompt is a hit; anything else is a false trigger.

`dsp/ROADMAP.md` (section "Node-side DSP improvements") lists these with the estimator side.
Tools: `dsp/tools/pipeline_cli` runs the node's C detector + estimator over a recording with
any settings; `dsp/tools/analyze_marks.py` scores angles per mark (it reads the original WAV
+ labels; ask Madden for those, or adapt it to these `.npz` files).
