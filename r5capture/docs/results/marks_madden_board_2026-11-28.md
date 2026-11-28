# Angle test at marked positions: Madden's board (2026-11-28)

Recording `marks_002114.wav/.txt/.labels` (on the NFS share, not in git): `record_marks.sh`,
5 taps at each of −90, −60, −30, 0, +30, +60, +90° at about 1 m, node hardware with 0.065 m
spacing (measured between boards), R5 sampler 20,000 frames/s, TI 6.1 kernel. Sampling clean:
0 skipped, 0 late, 0 ADC errors. First run of Joey's merged firmware on this kernel: works.

## Node DSP (defaults) per mark
| Mark | Detected | Accepted | Mean | Spread |
|---|---|---|---|---|
| −90 | 5 | 1 | +23.4 | |
| −60 | 5 | 2 | +18.1 | 21.7 |
| −30 | 5 | 5 | −5.1 | 1.4 |
| 0 | 5 | 5 | −4.1 | 0.9 |
| +30 | 5 | 5 | −26.4 | 3.4 |
| +60 | 5 | 4 | −32.5 | 1.5 |
| +90 | 5 | 4 | −35.3 | 5.3 |

## Raw delays (first 2 ms of each tap, sinc-interpolated correlation)
Fit over 32 taps (3 outliers with |lag| > 4 dropped): `lag = +1.74 · sin(mark) + 0.22` samples,
residual std 0.52 samples.
- **Sign reversed:** the "+" marks are on the ADC channel 0 (mic A) side.
- **Effective spacing 3.0 cm** instead of 6.5 cm, and not sinusoidal: −30 ≈ 0 ≈ −0.1 samples;
  +30, +60, +90 all ≈ +1.5 to +1.75. Same in the first 0.5 ms, so not late echoes.
  Joey's node at the same 6.5 cm reached the full ±3.8 samples (taps clamped at −90°).
- **Offset at 0°:** +0.22 samples measured vs −0.35 expected (−skew): +0.57 samples (29 µs).

## Suspected causes (untested)
1. Vibration through the table (node on the tapped table) reaching both mics almost at once.
2. Mic sound ports closer than the measured 6.5 cm.
3. Channel wiring vs labelling (sign).

## Next checks
Distance between sound ports; which mic is ch0 (scratch test); node isolated from the table
(`record_marks.sh 5 -90 0 90`); sound in the air instead of a table tap.

## Second run: claps in the air at the same marks (`marks_002638`)
Hands above each mark, ~1 m, 10-30 cm above the table. `dsp/tools/fit_marks.py`:

| | Taps (table) | Claps (air) |
|---|---|---|
| Fit | lag = +1.56·sin(mark) + 0.28 | lag = +2.68·sin(mark) − 0.58 |
| Effective spacing | 2.7 cm | **4.6 cm** |
| Per-mark mean lag −90…+90 | −1.66, −0.23, −1.07, −0.04, +1.46, +1.74, +1.68 | **−3.49, −2.77, −1.69, −0.66, +0.56, +2.06, +1.81** |
| Residual std | 1.37 samples | 0.43 samples |
| Sign | reversed | reversed |

- Air sounds follow a sine; table taps don't → **vibration through the table** distorts tap delays.
  The node must not sit on the surface being hit (or must be isolated from it).
- Sign reversed in both runs → swap channels (`DSP_SWAP_MICS=1`) or fix the labelling.
- Every clap clipped (peaks ~3300 counts on both channels). The ch0 (A) side saturates at ~+2
  samples (expected +3.4); the B side reaches −3.5 (expected −4.1).

Node DSP on the claps (`analyze_marks.py`, channels swapped offline):

| Settings | Mean abs error | RMS | Max |
|---|---|---|---|
| As run (6.5 cm, not swapped) | 81.0° | 92.7° | 145.8° |
| Swapped, 6.5 cm | 20.5° | 25.9° | 56.1° |
| Swapped, 4.6 cm (spacing fitted on this same recording: optimistic) | 11.6° | 15.7° | 37.9° |

Per-mark means with swap + 4.6 cm: −86.7, −77.7, −37.7, −8.7, +22.2, +63.3, +56.2 (true −90…+90).
