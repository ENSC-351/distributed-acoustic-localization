# First real recording: claps on Joey's board (2026-11-26)

Recording: `claps_232349.wav/.txt` (44 s, R5 sampler, 20,000 frames/s, kernel 7.2.8),
made with `node/board/record_claps.sh`: 5 s silence, 3 claps on the mic A side,
3 on the mic B side, 3 "in front". Mic spacing 0.27 m. Kept on the NFS share
(`myApps/integration/`), not in git (3.5 MB).

## Sampling
0 blocks skipped, 0 late, 0 ADC errors. Mic B sampled 0.3504 frames (17.52 us) after A.
Linux can read the R5's timer (`timer clock`).

## Signals
| | ch0 (mic A) | ch1 (mic B) |
|---|---|---|
| DC (whole recording, per second) | 1998.5 to 1999.6 | 2002.6 to 2005.5 |
| Noise std in silence | ~22 counts | ~2.3 counts |
| Quiet power | 47 % at 100-500 Hz, a 7.2 kHz tone | 60 % below 100 Hz |
| Clap peaks | always 3332-3348 | always 3623-3632 |

- No DC drift on either channel (Joey's earlier drift was not seen here).
- ch0 is about 10x noisier than ch1.
- Every clap stops at the same level on each channel: the mic amplifiers clip
  (well below the ADC's 4095). Claps are too loud for this gain.

## Pipeline, before and after tuning (same recording, offline, `dsp/tools/pipeline_cli`)
| Settings | Windows | False triggers | Directions | Side claps on the correct side |
|---|---|---|---|---|
| Original defaults (threshold 12x, holdoff 50 ms, 5+15 ms window, min peak/rms 7) | 216 | most of them | 1 | (claps lost in noise triggers) |
| Threshold 300x, holdoff 300 ms | 9 | 0 | 1 | |
| + window 2+5 ms, min peak/rms 3 | 9 | 0 | 6 | 3 of 3 accepted |
| + 500 Hz high-pass (new node default) | 9 | 0 | 6 | 4 of 4 accepted: -32.2, -36.8 (A), +53.8, +59.0 (B) |

- Claps trigger at 6,500-11,000x the noise floor; noise reached up to ~200x, so 300x separates them here.
- Peak-to-RMS of real claps is 3-6 (synthetic tests: ~20); the estimator's default of 7 rejects them.
- "In front" claps gave inconsistent angles (-90, +14): hand position wasn't controlled, and every clap clipped.
- Not measured: the true angles (positions weren't marked), so this checks sides, not accuracy.

## Caveats
One recording, one board, one person clapping. Thresholds tuned on it may not
suit marble impacts (quieter, different spectrum). Retune with
`pipeline_cli <file.csv> key=value ...` on each new recording.

## Next
- Marked positions (e.g. tape at -60, -30, 0, 30, 60 deg, 1 m) to measure angle error.
- Lower the mic gain or tap more softly to stop clipping.
- Find out why ch0 is 10x noisier (wiring, the mic, or its amplifier).
