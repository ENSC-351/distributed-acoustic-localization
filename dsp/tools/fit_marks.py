"""Physical check of a marked-position recording (node/board/record_marks.sh),
independent of the node's detector and estimator settings.

For every prompt: finds the impact onset, measures the delay between the
channels over the first 0.5 / 2 / 5 ms (500 Hz high-pass, sinc-interpolated
cross-correlation, lags within +-max_lag), then fits
    lag = slope * sin(mark) + intercept      (samples)
over all taps. From the fit:
    effective spacing = |slope| * c / fs
    sign: slope should be NEGATIVE (theta positive toward mic B = ch1, so B hears
          a +mark first and lag < 0); positive means channels or marks are swapped
    intercept should be -skew (B is sampled skew after A)
Usage: python fit_marks.py marks_HHMMSS.wav [spacing m] [max_lag]
"""
import math
import os
import sys
import wave

import numpy as np

C = 343.0


def highpass(v, fs, f0=500.0):
    w0 = 2 * math.pi * f0 / fs
    al = math.sin(w0) / (2 * 0.7071)
    c = math.cos(w0)
    a0 = 1 + al
    b0, b1, b2 = (1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0
    a1, a2 = -2 * c / a0, (1 - al) / a0
    y = np.zeros_like(v)
    x1 = x2 = y1 = y2 = 0.0
    for i, xi in enumerate(v):
        yi = b0 * xi + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, xi, y1, yi
        y[i] = yi
    return y


def lag_of(a, b, L, up=16):
    r = np.array([np.dot(a[L:len(a) - L], b[L + k:len(b) - L + k]) for k in range(-L, L + 1)])
    k = np.arange(-L, L + 1)
    fine = np.linspace(-L, L, 2 * L * up + 1)
    rf = np.array([np.sum(r * np.sinc(f - k)) for f in fine])
    return float(fine[int(np.argmax(rf))])


def main():
    path = sys.argv[1]
    spacing = float(sys.argv[2]) if len(sys.argv) > 2 else 0.065
    base = os.path.splitext(path)[0]
    w = wave.open(path)
    fs = w.getframerate()
    x = np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, 2).astype(float)
    max_lag = int(sys.argv[3]) if len(sys.argv) > 3 else int(math.ceil(spacing / C * fs)) + 3
    skew = 0.35
    if os.path.exists(base + ".txt"):
        for line in open(base + ".txt"):
            if line.startswith("ch1_offset_frames="):
                skew = float(line.split("=")[1])
    labels = [tuple(map(float, l.split())) for l in open(base + ".labels") if l.strip() and l[0] != "#"]
    a = highpass(x[:, 0] - x[:, 0].mean(), fs)
    b = highpass(x[:, 1] - x[:, 1].mean(), fs)
    q = slice(0, int(2.5 * fs))
    print(f"{os.path.basename(path)}: fs {fs}, nominal spacing {spacing} m (max lag {spacing / C * fs:.2f}), "
          f"skew {skew:.3f} samples; quiet std ch0 {x[q, 0].std():.1f} ch1 {x[q, 1].std():.1f}; "
          f"range ch0 {x[:, 0].min():.0f}..{x[:, 0].max():.0f} ch1 {x[:, 1].min():.0f}..{x[:, 1].max():.0f}")
    print(" mark | onset s | peak ch0 ch1 | lag 0.5ms  2ms   5ms")
    rows = []
    for mark, t in labels:
        s = int(t * fs)
        e = min(s + int(1.8 * fs), len(a))
        env = np.abs(a[s:e]) + np.abs(b[s:e])
        thr = 10 * np.median(env) + 50
        hit = np.flatnonzero(env > thr)
        if not len(hit):
            print(f" {mark:+4.0f} |   (no impact found)")
            continue
        o = s + int(hit[0])
        lags = []
        for dur in (0.0005, 0.002, 0.005):
            n = int(dur * fs)
            lags.append(lag_of(a[o - 40:o + n + 40], b[o - 40:o + n + 40], max_lag))
        pa, pb = np.abs(x[o:o + 400, 0] - x[:, 0].mean()).max(), np.abs(x[o:o + 400, 1] - x[:, 1].mean()).max()
        print(f" {mark:+4.0f} | {o / fs:7.3f} | {pa:5.0f} {pb:5.0f} | {lags[0]:+6.2f} {lags[1]:+6.2f} {lags[2]:+6.2f}")
        rows.append((mark, lags[1]))
    m = np.array([r[0] for r in rows])
    lag = np.array([r[1] for r in rows])
    ok = np.abs(lag) < max_lag - 0.5
    s = np.sin(np.radians(m[ok]))
    (slope, icpt), *_ = np.linalg.lstsq(np.vstack([s, np.ones_like(s)]).T, lag[ok], rcond=None)
    res = lag[ok] - (slope * s + icpt)
    print(f"\nfit over {ok.sum()} taps ({(~ok).sum()} at the lag limit dropped): "
          f"lag = {slope:+.2f} * sin(mark) {icpt:+.2f}, residual std {res.std():.2f} samples")
    print(f"  effective spacing {abs(slope) * C / fs * 100:.1f} cm (nominal {spacing * 100:.1f} cm)")
    print(f"  sign: {'OK (negative slope)' if slope < 0 else 'REVERSED: + marks are on the ch0 (mic A) side'}")
    print(f"  intercept {icpt:+.2f} vs expected {-skew:+.2f}: offset {icpt + skew:+.2f} samples "
          f"({(icpt + skew) / fs * 1e6:+.1f} us)")
    print("  per mark: " + ", ".join(f"{mk:+.0f}: {lag[ok & (m == mk)].mean():+.2f}" for mk in sorted(set(m))
                                     if (ok & (m == mk)).any()))


if __name__ == "__main__":
    main()
