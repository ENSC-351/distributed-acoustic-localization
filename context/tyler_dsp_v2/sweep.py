"""Detection rate and false-trigger rate versus noise level (synthetic data).

Usage: python3 sweep.py [trials]    Outputs: sweep_results.csv, sweep_plot.png
Peak SNR = 20*log10(impact amplitude / noise std). Two threshold ratios are
compared. Noise here is white Gaussian; real recordings will be worse
(ambient bumps, reflections), so tune the threshold on real data.
"""
import csv
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

from detector import DetectorConfig, detect_events
from synth import make_record

FS = 48000.0
AMPLITUDE = 1.0
TRUE_ONSET = int(0.08 * FS)
NOISE_LEVELS = [0.02, 0.05, 0.1, 0.2, 0.3, 0.5]
RATIOS = [12.0, 5.0]


def run(trials=100, ratios=RATIOS):
    rows = []
    for ratio in ratios:
        cfg = DetectorConfig(threshold_ratio=ratio)
        for sd in NOISE_LEVELS:
            hits, errs, false_pos, extra = 0, [], 0, 0
            for k in range(trials):
                ev = [e for e in detect_events(make_record(fs=FS, amplitude=AMPLITUDE, noise_std=sd, seed=k), FS, cfg) if e.valid]
                if ev:
                    err = ev[0].record_trigger_index - TRUE_ONSET
                    if 0 <= err <= 2 * 24:      # within two smoothing windows after the onset
                        hits += 1
                        errs.append(err)
                    extra += len(ev) - 1
                noise_only = [e for e in detect_events(make_record(fs=FS, impact=False, noise_std=sd, seed=10_000 + k), FS, cfg) if e.valid]
                false_pos += len(noise_only) > 0
            rows.append(dict(threshold_ratio=ratio, noise_std=sd,
                             peak_snr_db=round(20 * np.log10(AMPLITUDE / sd), 1), trials=trials,
                             detection_rate=hits / trials,
                             false_trigger_rate_noise_only=false_pos / trials,
                             extra_events_per_impact_record=extra / trials,
                             median_trigger_lag_samples=float(np.median(errs)) if errs else None,
                             worst_trigger_lag_samples=int(max(errs)) if errs else None))
    return rows


if __name__ == "__main__":
    rows = run(int(sys.argv[1]) if len(sys.argv) > 1 else 100)
    print(f"{'ratio':>5} {'noise':>6} {'SNR dB':>7} {'detect':>7} {'false+':>7} {'extra':>6} {'med lag':>8} {'worst':>6}")
    for r in rows:
        print(f"{r['threshold_ratio']:>5g} {r['noise_std']:>6} {r['peak_snr_db']:>7} {r['detection_rate']:>7.2f} "
              f"{r['false_trigger_rate_noise_only']:>7.2f} {r['extra_events_per_impact_record']:>6.2f} "
              f"{str(r['median_trigger_lag_samples']):>8} {str(r['worst_trigger_lag_samples']):>6}")
    with open("sweep_results.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=rows[0].keys()); w.writeheader(); w.writerows(rows)
    fig, ax = plt.subplots(figsize=(7.5, 4.2))
    for ratio, style in zip(RATIOS, ("o-", "s-")):
        sub = [r for r in rows if r["threshold_ratio"] == ratio]
        snr = [r["peak_snr_db"] for r in sub]
        ax.plot(snr, [r["detection_rate"] for r in sub], style, label=f"detection rate, ratio {ratio:g}")
        ax.plot(snr, [r["false_trigger_rate_noise_only"] for r in sub], style[0] + "--", alpha=0.6,
                label=f"false-trigger rate, ratio {ratio:g}")
    ax.set_xlabel("peak SNR (dB)"); ax.set_ylabel("rate"); ax.set_ylim(-0.05, 1.05)
    ax.set_title(f"Detector vs noise ({rows[0]['trials']} trials per point, synthetic)")
    ax.invert_xaxis(); ax.legend(fontsize=8); fig.tight_layout(); fig.savefig("sweep_plot.png", dpi=130)
    print("saved sweep_results.csv and sweep_plot.png")
