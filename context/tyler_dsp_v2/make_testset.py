"""Generate the synthetic known-delay test set (WAV + NPZ + manifest.json).

Usage: python3 make_testset.py [outdir]     (default: testdata)
delay = t_B - t_A in samples; positive = B hears it later.
WAV files are written with gain 0.5 so DC + impact fit in 16 bits; the
detector is scale-invariant, and the gain is recorded in the manifest.
"""
import json
import sys
from pathlib import Path

import numpy as np

from io_utils import write_wav
from synth import make_record

FS = 48000.0
WAV_GAIN = 0.5

CASES = {
    "delay_plus7":    dict(delay_samples=7),
    "delay_zero":     dict(delay_samples=0),
    "delay_minus7":   dict(delay_samples=-7),
    "delay_plus3":    dict(delay_samples=3),
    "delay_frac_2p5": dict(delay_samples=2.5),
    "low_snr":        dict(delay_samples=7, amplitude=0.4, noise_std=0.05),
    "no_impact":      dict(impact=False),
    "two_impacts":    dict(delay_samples=7, impact_time_s=[0.05, 0.14], duration_s=0.25),
}


def generate(outdir="testdata"):
    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)
    manifest = {}
    for i, (name, kw) in enumerate(CASES.items()):
        kw = dict(kw, fs=FS, seed=100 + i)
        data = make_record(**kw)
        np.savez(out / f"{name}.npz", data=data, fs=FS)
        write_wav(out / f"{name}.wav", data, FS, WAV_GAIN)
        manifest[name] = dict(
            fs=FS, wav_gain=WAV_GAIN, channel_order=["A (row 0)", "B (row 1)"],
            generator="synth.make_record",
            parameters={k: v for k, v in kw.items()}, samples_per_channel=int(data.shape[1]),
            expected_events=0 if kw.get("impact") is False else len(np.atleast_1d(kw.get("impact_time_s", 0.08))),
            expected_delay_samples=None if kw.get("impact") is False else kw.get("delay_samples", 7.0))
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2))
    return manifest


if __name__ == "__main__":
    m = generate(sys.argv[1] if len(sys.argv) > 1 else "testdata")
    print(f"wrote {len(m)} cases (.npz + .wav each) and manifest.json")
