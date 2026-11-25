# 2. Direction finding with two microphones (TDOA)

## The geometry

Mic A at x = −d/2, mic B at x = +d/2 (d = spacing). A sound from far away arrives as a
flat wavefront at angle θ (0 = straight ahead, "broadside"; positive toward B). The
extra distance to the farther mic is `d·sin θ`, so the **time difference of arrival** is

```
tdoa = t_B − t_A = −d·sin(θ) / c          (c = 343 m/s)
θ   = asin(−c · tdoa / d)
```

Sign convention (pinned by tests): **positive tdoa means A heard it first**.

**Worked example.** d = 10 cm, sound from θ = −30° (toward A):
tdoa = −0.10 · sin(−30°) / 343 = +146 µs. At 48,000 samples/s that's 7.0 samples,
which is exactly Tyler's `delay_plus7` test case; the estimator returned −29.8°.

Max |tdoa| is d/c (sound along the axis): 0.29 ms at 10 cm.

**Front/back ambiguity.** A sound at θ in front and the mirror position behind give the
same delay. Two mics can't tell them apart, so every EVENT carries `THETA_MIRROR`.
A third mic (the MCP3208 has 8 channels) would remove this.

**Far-field assumption.** The formula assumes a flat wavefront, i.e. the source is far
compared with d. Close sources (≲ a few × d) give curved wavefronts and biased angles.

## Finding the delay: cross-correlation

Slide channel B against A and measure how well they match at each shift (lag) k:

```
r[k] = Σ a[n] · b[n + k]
```

If B is A delayed by D samples, r peaks at k = +D. The estimator only searches the
**physically possible** lags, |k| ≤ fs·d/c, plus a small tolerance.

**Sub-sample refinement.** The true delay is rarely a whole number of samples. A
parabola through the peak and its two neighbours gives a fractional peak position
(e.g. the 2.5-sample test case gave 2.57).

**Channel skew correction.** The MCP3208 samples B slightly after A (see file 3), so
`tdoa = peak_lag/fs + channel_skew_s`, with skew = sampling time of B − sampling time of A.
Correct it in exactly one place; correcting twice is as wrong as not at all.

**Cost.** Correlating all lags of a 960-sample window is ~1 million multiply-adds:
~7 ms on the BeagleY-AI. Only ~2·(fs·d/c)+1 lags are physically possible (about 11 at
17 kHz, 10 cm), so computing only those would be over 100× cheaper (11 lags instead of 1,919). (Backlog item.)

## GCC-PHAT (the alternative)

Compute the correlation in the frequency domain and divide each frequency by its
magnitude (PHAT = "phase transform"), so every frequency counts equally. It sharpens
peaks for broadband sounds and resists reverberation. On our tests it did **worse**:
- broadband noise bursts at noise 0.3: GCC-PHAT accepted 3/100, plain xcorr 38/100;
- Tyler's tonal 1.5 kHz impacts: GCC-PHAT off by 0.3–0.7 samples and refused low SNR.

Why: our test impacts are narrowband, and PHAT gives the noise-only frequencies as much
weight as the signal. Decision: plain cross-correlation for now; revisit on real recordings
(partial whitening "PHAT-β" or band-limiting are the usual fixes).

## Refusing bad answers

An estimator that always returns an angle will return confident nonsense for noise. Ours
returns a **status** and only gives an angle when OK:

| Status | Meaning |
|---|---|
| LOW_ENERGY | A channel is (nearly) silent |
| NO_PEAK | No positive correlation peak in the allowed range |
| PEAK_OUT_OF_RANGE | The strongest peak overall is at an impossible lag |
| TDOA_OUT_OF_RANGE | Corrected delay exceeds d/c + tolerance |
| WEAK_PEAK | Peak-to-RMS of the correlation below `min_peak_to_rms` (looks like noise) |

**Peak-to-RMS.** Pure noise also produces in-range peaks: without a check, the estimator
returned confident angles for noise in 3/16 trials. Noise-only windows never exceeded
5.7 peak-to-RMS in 200 trials; real estimates were mostly 15–45; threshold set to **7**.
Tyler's tonal windows only reached ~7.8, barely above it, so this **must be retuned on
real recordings**.

**Confidence** = 1 − second_peak / main_peak. A heuristic, not a probability. The
`AMBIGUOUS` flag is set when second/main ≥ 0.8. In the Milestone 2 demo the low-SNR case
was wrong by 4.1° but had confidence 0.02 and AMBIGUOUS: the bad answer was labelled as bad.

## Results on synthetic data (Tyler's test set, 48 kHz)

| Case | True delay | xcorr | GCC-PHAT |
|---|---|---|---|
| delay +7 | 7 | 6.95 | 6.40 |
| delay 0 | 0 | −0.04 | 0.40 |
| delay −7 | −7 | −6.98 | −6.98 |
| delay +3 | 3 | 3.03 | 3.67 |
| fractional 2.5 | 2.5 | 2.57 | 2.79 |
| low SNR | 7 | 7.85 (AMBIGUOUS) | refused (weak peak) |
| two impacts | 7, 7 | 7.04, 7.05 | 7.26, 6.92 |

Identical on Windows, x86 Linux and the BeagleY-AI (0 differences in 32 results).
