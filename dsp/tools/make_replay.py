"""Make a synthetic two-mic recording for the node's replay mode (DSP_REPLAY).

Writes "a,b" per line in ADC counts (12-bit, idle ~2048): 1.5 s of quiet
(the detector calibrates on the first second), then one decaying noise burst
every 0.5 s, mic B delayed by a known number of samples. Prints the delay
and the expected angle of each burst, so the master's output can be checked.

Synthetic data only: real impacts, rooms and microphones will differ.

Usage: python3 make_replay.py out.csv [rate Hz] [spacing m]
No numpy needed.
"""
import math
import random
import sys

C = 343.0


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "replay.csv"
    fs = float(sys.argv[2]) if len(sys.argv) > 2 else 20000.0
    spacing = float(sys.argv[3]) if len(sys.argv) > 3 else 0.20
    delays = [0, 3, -3, 6, -6, 9, -9]   # samples B lags A
    rng = random.Random(351)
    quiet = int(1.5 * fs)
    gap = int(0.5 * fs)
    n = quiet + gap * len(delays) + int(0.2 * fs)
    a = [2048.0 + rng.uniform(-3, 3) for _ in range(n)]
    b = [2048.0 + rng.uniform(-3, 3) for _ in range(n)]
    print(f"{out}: {n} frames at {fs:.0f} Hz, spacing {spacing} m")
    for k, d in enumerate(delays):
        onset = quiet + k * gap
        for i in range(int(0.03 * fs)):
            v = 600.0 * rng.uniform(-1, 1) * math.exp(-i / (0.004 * fs))
            if onset + i < n:
                a[onset + i] += v
            if 0 <= onset + i + d < n:
                b[onset + i + d] += v
        theta = math.degrees(math.asin(max(-1.0, min(1.0, -C * (d / fs) / spacing))))
        print(f"  burst {k} at {onset / fs:.3f} s: B lags A by {d:+d} samples -> theta {theta:+.1f} deg")
    with open(out, "w") as f:
        f.write("# synthetic replay for DSP_REPLAY: a,b in ADC counts\n")
        for x, y in zip(a, b):
            f.write(f"{int(round(x))},{int(round(y))}\n")


if __name__ == "__main__":
    main()
