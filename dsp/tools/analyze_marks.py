"""Angle accuracy from a marked-position recording (node/board/record_marks.sh).

Runs the node's DSP (pipeline_cli, same code and settings as the node) over the
recording, matches every detected window to the tap prompt just before it, and
prints per mark: taps prompted, detected, accepted, mean angle, error, spread.

Usage:
  python analyze_marks.py <pipeline_cli> marks_HHMMSS.wav [spacing m] [key=value ...]
  (spacing default 0.065; extra key=value pairs go to pipeline_cli, e.g. min_ptr=2)
Needs numpy. The labels file is found next to the .wav.
"""
import os
import subprocess
import sys
import tempfile
import wave

import numpy as np

NODE_DEFAULTS = ["hp=500", "threshold=300", "holdoff_ms=300", "pre_ms=2", "post_ms=5", "min_ptr=3"]
MATCH_S = 1.5   # a window belongs to the latest prompt up to this long before it


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cli, wav_path = sys.argv[1], sys.argv[2]
    spacing = sys.argv[3] if len(sys.argv) > 3 and "=" not in sys.argv[3] else "0.065"
    extra = [a for a in sys.argv[3:] if "=" in a]
    base = os.path.splitext(wav_path)[0]
    labels = [tuple(float(v) for v in l.split()) for l in open(base + ".labels") if l.strip() and l[0] != "#"]
    skew_us = "17.52"
    if os.path.exists(base + ".txt"):
        for l in open(base + ".txt"):
            if l.startswith("ch1_offset_ticks="):
                skew_us = f"{int(l.split('=')[1]) / 25.0:.2f}"   # 25 MHz R5 timer
    w = wave.open(wav_path)
    fs = w.getframerate()
    x = np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, 2)
    with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as f:
        np.savetxt(f, x, fmt="%d", delimiter=",")
        csv = f.name
    try:
        args = [cli, csv, f"fs={fs}", f"spacing={spacing}", f"skew_us={skew_us}"] + NODE_DEFAULTS + extra
        out = subprocess.run(args, capture_output=True, text=True, check=True).stdout.splitlines()
    finally:
        os.unlink(csv)

    wins = []
    for line in out:
        if line.startswith("#") or not line.strip():
            continue
        t = float(line.split()[0])
        ok = line.split()[1] == "ok"
        theta = float(line.split("theta")[1].split()[0]) if ok else None
        wins.append((t, ok, theta))

    per = {}
    prompts = sorted(labels, key=lambda l: l[1])
    used = set()
    false_det = 0
    for t, ok, theta in wins:
        before = [p for p in prompts if p[1] <= t + 0.05 and t - p[1] < MATCH_S]
        if not before:
            false_det += 1
            continue
        p = before[-1]
        key = (p[0], p[1])
        if key in used:
            false_det += 1   # second window for one prompt
            continue
        used.add(key)
        per.setdefault(p[0], []).append((ok, theta))

    print(f"{os.path.basename(wav_path)}: fs {fs}, spacing {spacing} m, skew {skew_us} us; "
          f"settings: {' '.join(NODE_DEFAULTS + extra)}")
    print(" mark   prompted detected accepted   mean    error   spread(std)  min..max")
    all_err = []
    marks = sorted({p[0] for p in prompts})
    for m in marks:
        n = sum(1 for p in prompts if p[0] == m)
        got = per.get(m, [])
        th = [t for ok, t in got if ok]
        if th:
            mean, sd = float(np.mean(th)), float(np.std(th))
            all_err += [t - m for t in th]
            print(f" {m:+5.0f}   {n:6d} {len(got):8d} {len(th):8d}   {mean:+6.1f}  {mean - m:+6.1f}   {sd:6.1f}     "
                  f"{min(th):+.0f}..{max(th):+.0f}")
        else:
            print(f" {m:+5.0f}   {n:6d} {len(got):8d} {0:8d}      --")
    if all_err:
        e = np.abs(all_err)
        print(f"all accepted taps: mean |error| {e.mean():.1f} deg, RMS {np.sqrt(np.mean(np.square(all_err))):.1f} deg, "
              f"max {e.max():.1f} deg; detections not matching a prompt: {false_det}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
