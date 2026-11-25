#!/usr/bin/env python3
"""Analyze a two-channel ADC recording made with linux/adc_record.

    python3 analyze_recording.py rec.wav rec.txt [--same-mic]

Reports, per channel: DC level, noise, range, clipping. Timing: sample rate and
gaps between SPI messages. For each tap/impact found: dominant frequency (and the
mic spacing it allows, c/(2f)) and, with --same-mic (one mic wired to ch0 and ch1),
the measured delay between the channels, which must equal the MCP3208's multiplexer
skew: ch1 is converted one conversion after ch0 (half a frame for Linux SPI
messages; for the R5 the firmware reports it as ch1_offset_frames).

Delay convention matches the C estimator: r[k] = sum a[n] * b[n+k]; with b sampled
later by `skew`, the peak is at k = -skew/T, and tdoa = k*T + skew should be ~0.
"""
import sys
import wave

import numpy as np

C = 343.0                 # speed of sound, m/s
ADC_MAX = 4095
EVENT_WINDOW_S = 0.020    # analyze 20 ms per impact
PRE_ONSET_S = 0.002
MIN_EVENT_SPACING_S = 0.25
MAX_EVENTS = 12
MAX_LAG = 4               # samples searched for the ch0/ch1 delay


def load(wav_path, txt_path):
    with wave.open(wav_path, "rb") as w:
        if w.getnchannels() != 2 or w.getsampwidth() != 2:
            sys.exit("expected a 2-channel 16-bit WAV from adc_record")
        raw = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(float)
    data = raw.reshape(-1, 2)
    meta, blocks = {}, []
    for line in open(txt_path):
        line = line.strip()
        if line.startswith("block,"):
            _, i, first, t0, t1 = line.split(",")
            blocks.append((int(first), int(t0), int(t1)))
        elif "=" in line:
            k, v = line.split("=", 1)
            meta[k] = v
    return data, meta, np.array(blocks, dtype=np.int64)


def noise_rms(x, frames_per_window):
    """RMS of the quietest 10 % of windows: background noise, ignoring the taps."""
    n = len(x) // frames_per_window
    if n == 0:
        return float(np.std(x))
    stds = np.std(x[: n * frames_per_window].reshape(n, frames_per_window), axis=1)
    return float(np.percentile(stds, 10))


def find_events(x, dc, noise, fs):
    env = np.abs(x - dc)
    threshold = max(8 * noise, 0.2 * env.max())
    events, i, gap = [], 0, int(MIN_EVENT_SPACING_S * fs)
    above = np.nonzero(env > threshold)[0]
    for idx in above:
        if not events or idx - events[-1] > gap:
            events.append(int(idx))
            if len(events) >= MAX_EVENTS:
                break
    return events, threshold


def dominant_frequency(seg, fs):
    seg = seg - seg.mean()
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n=8192))
    freqs = np.fft.rfftfreq(8192, 1 / fs)
    spec[freqs < 50] = 0                       # ignore DC drift
    power = spec ** 2
    cumulative = np.cumsum(power) / power.sum()
    f90 = freqs[np.searchsorted(cumulative, 0.90)]
    centroid = float((freqs * power).sum() / power.sum())
    return float(freqs[np.argmax(spec)]), centroid, float(f90)


def channel_lag(a, b):
    """Peak of r[k] = sum a[n] b[n+k], with parabolic sub-sample refinement."""
    a = a - a.mean()
    b = b - b.mean()
    n = len(a)
    lags = np.arange(-MAX_LAG, MAX_LAG + 1)
    r = np.array([np.dot(a[max(0, -k): n - max(0, k)], b[max(0, k): n - max(0, -k)]) for k in lags])
    i = int(np.argmax(r))
    if 0 < i < len(r) - 1:
        y0, y1, y2 = r[i - 1], r[i], r[i + 1]
        denom = y0 - 2 * y1 + y2
        frac = 0.5 * (y0 - y2) / denom if denom != 0 else 0.0
    else:
        frac = 0.0
    return lags[i] + frac


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    same_mic = "--same-mic" in sys.argv
    data, meta, blocks = load(sys.argv[1], sys.argv[2])
    fs = float(meta.get("long_run_rate_hz", 0)) or 1.0
    frames = len(data)
    print(f"Recording: {frames} frames, {frames / fs:.2f} s, {meta.get('transfer_mode', '?')}")

    print("\n== Timing")
    print(f"  long-run rate per mic:     {fs:9.0f} samples/s   (frames / elapsed time)")
    print(f"  in-block rate, upper bound:{float(meta.get('inblock_rate_hz_upper_bound', 0)):9.0f} samples/s"
          f"   ({meta.get('frames_per_block')} frames per SPI message)")
    if len(blocks) > 1:
        gaps_us = (blocks[1:, 1] - blocks[:-1, 2]) / 1000.0
        print(f"  gaps between SPI messages: mean {gaps_us.mean():.1f} us, median {np.median(gaps_us):.1f} us,"
              f" p99 {np.percentile(gaps_us, 99):.1f} us, max {gaps_us.max():.1f} us")
        busy = (blocks[:, 2] - blocks[:, 1]).sum() / (blocks[-1, 2] - blocks[0, 1])
        print(f"  time inside SPI messages:  {100 * busy:.2f} %  (the rest is between messages)")
    print(f"  ADC errors: {meta.get('adc_errors', '?')}   real-time priority: {meta.get('realtime_priority', '?')}")

    window = max(1, int(0.02 * fs))
    print("\n== Levels (raw ADC counts, 0..4095)")
    dcs, noises = [], []
    for ch in range(2):
        x = data[:, ch]
        dc = float(np.median(x))
        nz = noise_rms(x, window)
        clipped = int(np.sum((x <= 0) | (x >= ADC_MAX)))
        dcs.append(dc)
        noises.append(nz)
        print(f"  ch{ch}: DC {dc:7.1f}   noise {nz:5.2f} counts RMS   range {x.min():.0f}..{x.max():.0f}"
              f"   clipped samples {clipped}")

    events, threshold = find_events(data[:, 0], dcs[0], noises[0], fs)
    print(f"\n== Impacts on ch0 (threshold {threshold:.0f} counts above DC): {len(events)} found")
    if not events:
        print("  none: tap or drop something near the mic during the recording")
        return
    pre, length = int(PRE_ONSET_S * fs), int(EVENT_WINDOW_S * fs)
    lags, doms = [], []
    print(f"  {'#':>2} {'time s':>7} {'peak':>6} {'dominant Hz':>12} {'centroid Hz':>12} {'90% below Hz':>13}"
          + (f" {'ch1 lag (samples)':>18}" if same_mic else ""))
    for n, onset in enumerate(events):
        s0 = max(0, onset - pre)
        seg0 = data[s0:s0 + length, 0]
        seg1 = data[s0:s0 + length, 1]
        if len(seg0) < length // 2:
            continue
        dom, centroid, f90 = dominant_frequency(seg0, fs)
        doms.append(dom)
        line = (f"  {n:>2} {onset / fs:7.2f} {np.abs(seg0 - dcs[0]).max():6.0f} {dom:12.0f} {centroid:12.0f}"
                f" {f90:13.0f}")
        if same_mic:
            lag = channel_lag(seg0, seg1)
            lags.append(lag)
            line += f" {lag:18.3f}"
        print(line)

    dom = float(np.median(doms))
    print(f"\n  median dominant frequency: {dom:.0f} Hz -> max mic spacing without wrong-cycle risk"
          f" ~ c/(2f) = {100 * C / (2 * dom):.1f} cm")
    print(f"  Nyquist (fs/2) = {fs / 2:.0f} Hz: energy above it aliases (no anti-aliasing filter yet)")

    if same_mic and lags:
        lags = np.array(lags)
        period_us = 1e6 / float(meta.get("inblock_rate_hz_upper_bound") or fs)
        # How long after ch0 ch1 is sampled, in frames. The R5 firmware measures it
        # (conversions back to back at the start of each frame); for Linux SPI
        # messages the conversions are evenly spaced, so it's half a frame.
        offset_frames = float(meta.get("ch1_offset_frames", 0.5))
        expected = -offset_frames
        print("\n== Channel skew (same mic on ch0 and ch1)")
        print(f"  measured lag: {lags.mean():+.3f} samples (sd {lags.std():.3f}, {len(lags)} impacts)")
        print(f"  expected:     {expected:+.3f} samples (ch1 sampled {offset_frames:.3f} frame later)")
        print(f"  = {-lags.mean() * period_us:.1f} us measured skew vs ~{offset_frames * period_us:.1f} us expected"
              f" (frame period ~{period_us:.1f} us)")
        print(f"  residual after skew correction: {lags.mean() - expected:+.3f} samples"
              f" -> {(lags.mean() - expected) * period_us:+.2f} us (should be ~0)")


if __name__ == "__main__":
    main()
