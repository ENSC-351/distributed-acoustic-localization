"""Synthetic recordings for three nodes hearing the same sounds, for testing the
whole chain (node DSP -> network -> master localizer/fusion) without boards.

Geometry matches the master's layout (master/src/layout.c on madden/feature/web-gui):
an equilateral triangle of side SIDE metres centred on the origin; node 1 at the top,
2 bottom-left, 3 bottom-right; each node's broadside points at the centre; mic B is on
the node's right when facing the centre. Map angles are degrees CCW from +x.

For each sound position (x, y), each mic gets the same decaying noise burst, delayed by
its exact distance / c (fractional-sample delays applied in the frequency domain) and
scaled by 1/distance. Both channels of a node are sampled at the same instant (no ADC
skew), as the node's replay mode assumes. Synthetic data only.

Usage:
  python make_triangle_replay.py OUTDIR [--side 2.0] [--spacing 0.065] [--fs 20000]
                                        [--sources "x,y x,y ..."]
Writes OUTDIR/node1.csv, node2.csv, node3.csv and OUTDIR/truth.txt (one line per sound:
time_s x y theta1 theta2 theta3, the theta each node should report).
"""
import argparse
import math
import os

import numpy as np

C = 343.0
QUIET_S = 1.5          # the detector calibrates on the first second
GAP_S = 1.0            # between sounds (longer than the node's holdoff and fusion window)
BURST_LEN = 4096       # samples per generated burst segment
LEAD = 64              # samples of margin before a burst inside its segment


def layout(side):
    r = side / math.sqrt(3.0)
    c30, s30 = math.cos(math.radians(30)), 0.5
    return [  # id, x, y, heading (deg CCW from +x, toward the centre)
        (1, 0.0, r, -90.0),
        (2, -r * c30, -r * s30, 30.0),
        (3, r * c30, -r * s30, 150.0),
    ]


def mic_positions(node, spacing):
    _, x, y, heading = node
    right = math.radians(heading - 90.0)            # mic B is on the node's right
    ux, uy = math.cos(right), math.sin(right)
    a = (x - ux * spacing / 2, y - uy * spacing / 2)
    b = (x + ux * spacing / 2, y + uy * spacing / 2)
    return a, b


def wrap(deg):
    while deg > 180.0:
        deg -= 360.0
    while deg <= -180.0:
        deg += 360.0
    return deg


def expected_theta(node, sx, sy):
    _, x, y, heading = node
    bearing = math.degrees(math.atan2(sy - y, sx - x))
    return wrap(heading - bearing)                   # layout: bearing = heading - theta


def burst(rng, fs):
    n = np.arange(BURST_LEN)
    b = rng.uniform(-1, 1, BURST_LEN) * np.exp(-n / (0.004 * fs))
    spec = np.fft.rfft(b)
    f = np.fft.rfftfreq(BURST_LEN, 1 / fs)
    spec[f > 0.4 * fs] = 0.0                         # band-limit so fractional delays are clean
    return spec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--side", type=float, default=2.0)
    ap.add_argument("--spacing", type=float, default=0.065)
    ap.add_argument("--fs", type=float, default=20000.0)
    ap.add_argument("--sources", default="0,0 0,0.5 -0.4,-0.2 0.4,-0.2 0.2,0.15 -0.25,0.3 0,-0.45")
    args = ap.parse_args()
    fs = args.fs
    nodes = layout(args.side)
    sources = [tuple(float(v) for v in s.split(",")) for s in args.sources.split()]
    n_total = int((QUIET_S + GAP_S * len(sources) + 0.5) * fs)
    rng = np.random.default_rng(351)
    chans = {n[0]: np.zeros((n_total, 2)) for n in nodes}
    freqs = np.fft.rfftfreq(BURST_LEN, 1 / fs)
    truth = []

    for k, (sx, sy) in enumerate(sources):
        t0 = QUIET_S + k * GAP_S
        spec = burst(rng, fs)
        thetas = []
        for node in nodes:
            for ch, (mx, my) in enumerate(mic_positions(node, args.spacing)):
                dist = math.hypot(sx - mx, sy - my)
                arrival = (t0 + dist / C) * fs           # in samples
                start = int(math.floor(arrival)) - LEAD
                frac = arrival - start                   # LEAD <= frac < LEAD + 1
                seg = np.fft.irfft(spec * np.exp(-2j * np.pi * freqs * frac / fs), BURST_LEN)
                amp = 700.0 * min(1.0, 0.5 / max(dist, 0.05))
                end = min(start + BURST_LEN, n_total)
                chans[node[0]][start:end, ch] += amp * seg[: end - start]
            thetas.append(expected_theta(node, sx, sy))
        truth.append((t0, sx, sy, *thetas))

    os.makedirs(args.outdir, exist_ok=True)
    for nid, data in chans.items():
        noise = rng.uniform(-3, 3, data.shape)
        counts = np.clip(np.round(2048 + data + noise), 0, 4095).astype(int)
        path = os.path.join(args.outdir, f"node{nid}.csv")
        with open(path, "w") as f:
            f.write(f"# node {nid}: synthetic triangle replay, side {args.side} m, spacing {args.spacing} m\n")
            np.savetxt(f, counts, fmt="%d", delimiter=",")
    with open(os.path.join(args.outdir, "truth.txt"), "w") as f:
        f.write(f"# side {args.side} spacing {args.spacing} fs {fs}\n# time_s x y theta1 theta2 theta3\n")
        for row in truth:
            f.write(" ".join(f"{v:.4f}" for v in row) + "\n")
    print(f"{args.outdir}: {len(sources)} sounds, side {args.side} m, spacing {args.spacing} m")
    for t0, sx, sy, a, b, c in truth:
        print(f"  t={t0:5.2f} s  ({sx:+.2f}, {sy:+.2f}) m  theta: node1 {a:+6.1f}  node2 {b:+6.1f}  node3 {c:+6.1f}")


if __name__ == "__main__":
    main()
