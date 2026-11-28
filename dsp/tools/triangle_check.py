"""Checks a three-node run: reads the master's log ("Node N event K: theta T"),
groups the reports of each sound, intersects the bearing lines (least squares, the
same method as master/src/locate.c) and compares with truth.txt from
make_triangle_replay.py.

Usage: python triangle_check.py truth.txt master.log
Reports per sound are matched to the truth in order of arrival per node.
"""
import math
import re
import sys

import numpy as np


def layout(side):
    r = side / math.sqrt(3.0)
    c30 = math.cos(math.radians(30))
    return {1: (0.0, r, -90.0), 2: (-r * c30, -r * 0.5, 30.0), 3: (r * c30, -r * 0.5, 150.0)}


def intersect(lines):
    # minimise sum of squared perpendicular distances to the lines
    a = np.zeros((2, 2))
    b = np.zeros(2)
    for x, y, bearing in lines:
        d = np.array([math.cos(math.radians(bearing)), math.sin(math.radians(bearing))])
        p = np.eye(2) - np.outer(d, d)
        a += p
        b += p @ np.array([x, y])
    return np.linalg.solve(a, b)


def main():
    truth_path, log_path = sys.argv[1], sys.argv[2]
    side = 2.0
    truth = []
    for line in open(truth_path):
        if line.startswith("# side"):
            side = float(line.split()[2])
        elif not line.startswith("#") and line.strip():
            truth.append([float(v) for v in line.split()])
    per_node = {1: [], 2: [], 3: []}
    for line in open(log_path):
        m = re.search(r"Node (\d+) event \d+: theta (-?[\d.]+)", line)
        if m and int(m.group(1)) in per_node:
            per_node[int(m.group(1))].append(float(m.group(2)))
    nodes = layout(side)
    print(f"side {side} m; reports per node: " + ", ".join(f"{k}: {len(v)}" for k, v in per_node.items()))
    print(" sound  true (x, y)      theta err n1/n2/n3 (deg)   located (x, y)    error (cm)")
    errs = []
    for i, row in enumerate(truth):
        _, tx, ty, *want = row
        got = [per_node[n][i] if i < len(per_node[n]) else None for n in (1, 2, 3)]
        lines = [(nodes[n][0], nodes[n][1], nodes[n][2] - g) for n, g in zip((1, 2, 3), got) if g is not None]
        dth = "/".join(f"{g - w:+5.1f}" if g is not None else "  -- " for g, w in zip(got, want))
        if len(lines) >= 2:
            x, y = intersect(lines)
            e = math.hypot(x - tx, y - ty) * 100
            errs.append(e)
            print(f"  {i:3d}   ({tx:+.2f}, {ty:+.2f})   {dth}    ({x:+.3f}, {y:+.3f})   {e:6.1f}")
        else:
            print(f"  {i:3d}   ({tx:+.2f}, {ty:+.2f})   {dth}    (fewer than 2 lines)")
    if errs:
        print(f"position error: mean {np.mean(errs):.1f} cm, max {np.max(errs):.1f} cm over {len(errs)} sounds")


if __name__ == "__main__":
    main()
