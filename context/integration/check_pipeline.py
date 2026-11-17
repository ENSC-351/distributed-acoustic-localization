"""Integration check: Tyler's detector -> Madden's C estimator, against manifest.json.

Usage: python -I check_pipeline.py TYLER_DSP_DIR TDOA_CLI OUT_DIR
"""
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

tyler_dir, cli, out_dir = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
sys.path.insert(0, str(tyler_dir))
from detector import detect_events          # noqa: E402  (Tyler's code, read before running)
from io_utils import load_recording         # noqa: E402

out_dir.mkdir(parents=True, exist_ok=True)
manifest = json.loads((tyler_dir / "testdata" / "manifest.json").read_text())

SPACINGS = (0.10, 0.30)          # placeholders: real spacing is TBD
METHODS = ("xcorr", "gcc")
C = 343.0


def run_cli(csv, fs, spacing, method, extra=()):
    p = subprocess.run([cli, "--fs", str(fs), "--spacing", str(spacing), "--method", method,
                        *extra, str(csv)], capture_output=True, text=True)
    if p.returncode == 2:
        raise RuntimeError(p.stderr)
    return json.loads(p.stdout)


rows = []
print(f"{'case':15} {'ev':>2} {'expect':>6} | " +
      " | ".join(f"{m:5} d={d:.2f}: lag   status" for d in SPACINGS for m in METHODS))
for name, info in manifest.items():
    data, fs = load_recording(tyler_dir / "testdata" / f"{name}.wav")
    events = [e for e in detect_events(data, fs) if e.valid]
    expect = info["expected_delay_samples"]
    if len(events) != info["expected_events"]:
        print(f"{name:15} detector found {len(events)} events, manifest expects {info['expected_events']}")
    if not events:
        print(f"{name:15}  - {'none':>6} | (no window to estimate)")
        continue
    for k, ev in enumerate(events):
        csv = out_dir / f"{name}_event{k}.csv"
        np.savetxt(csv, np.column_stack([ev.samples_a, ev.samples_b]), delimiter=",",
                   header="samples_a,samples_b (Tyler: DC removed from pre-trigger median)")
        cells = []
        for d in SPACINGS:
            for m in METHODS:
                r = run_cli(csv, fs, d, m, ["--no-dc"])   # Tyler already removed DC
                lag = r["raw_lag_samples"]
                err = None if (lag is None or expect is None) else lag - expect
                rows.append(dict(case=name, event=k, spacing=d, expected=expect,
                                 n=ev.sample_count, **r, lag_error=err))
                cells.append(f"{(f'{lag:6.2f}' if lag is not None else '   n/a')} {r['status'][:22]:22}")
        print(f"{name:15} {k:>2} {str(expect):>6} | " + " | ".join(cells))

(out_dir / "results.json").write_text(json.dumps(rows, indent=2))
print(f"\nwindow length {events[0].sample_count if events else '?'} samples at {fs:g} Hz;"
      f" max delay = {SPACINGS[0]*fs/C:.1f} samples at d={SPACINGS[0]}, {SPACINGS[1]*fs/C:.1f} at d={SPACINGS[1]}")
