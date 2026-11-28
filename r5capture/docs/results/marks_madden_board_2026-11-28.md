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
