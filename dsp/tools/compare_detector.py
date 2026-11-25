"""Check the C streaming detector against Tyler's detector.py.

For each recording, runs detector.py's detect_events() with a fixed noise
model (the same DC and floor it would compute from the whole record), then
runs detector_cli with that noise model at several block sizes, and compares
trigger frame, window start and window DC.

Usage:
    python compare_detector.py <detector_cli> <dir with detector.py> <recordings...>
Recordings: .npz with 'data' (2, N) and 'fs' (Tyler's testdata format).
Exit code: number of mismatches.
"""
import os
import subprocess
import sys
import tempfile

import numpy as np


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    cli, tyler_dir, paths = sys.argv[1], sys.argv[2], sys.argv[3:]
    sys.path.insert(0, tyler_dir)
    import detector as tyler   # noqa: E402  (Tyler's module)

    cfg = tyler.DetectorConfig()
    mismatches = 0
    for path in paths:
        rec = np.load(path)
        data = np.asarray(rec["data"], dtype=float)
        fs = float(rec["fs"])

        # The noise model detector.py builds from the whole record
        smooth = max(1, int(round(cfg.smooth_ms * 1e-3 * fs)))
        dc = np.median(data, axis=1)
        floor = float(np.nanmedian(tyler.combined_energy(data - dc[:, None], smooth)))
        noise = tyler.NoiseModel((float(dc[0]), float(dc[1])), floor)
        ref = tyler.detect_events(data, fs, cfg, noise_model=noise)
        # Windows that run past the end never complete in a stream
        ref = [e for e in ref if e.status != "window extends past record edge"
               or e.record_trigger_index - int(round(cfg.pre_ms * 1e-3 * fs)) < 0]

        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as f:
            np.savetxt(f, data.T.astype(np.float32), fmt="%.9g", delimiter=",")
            csv = f.name
        try:
            for block in (1, 37, 128, 100000):
                out = subprocess.run([cli, csv, repr(fs), repr(float(dc[0])), repr(float(dc[1])), repr(floor),
                                      str(block)], capture_output=True, text=True, check=True).stdout.split("\n")
                got = [line.split() for line in out if line.strip()]
                name = os.path.basename(path)
                if len(got) != len(ref):
                    print(f"MISMATCH {name} block {block}: {len(got)} windows, detector.py {len(ref)}")
                    mismatches += 1
                    continue
                for g, e in zip(got, ref):
                    ok = (g[0] == "ok") == e.valid and int(g[1]) == e.record_trigger_index
                    if e.valid:
                        dc_e = e.diagnostics["window_dc_estimate"]
                        ok = ok and int(g[2]) == e.window_start
                        ok = ok and abs(float(g[4]) - dc_e[0]) < 1e-4 * max(1.0, abs(dc_e[0]))
                        ok = ok and abs(float(g[5]) - dc_e[1]) < 1e-4 * max(1.0, abs(dc_e[1]))
                    if not ok:
                        print(f"MISMATCH {name} block {block}: C {g} vs detector.py valid={e.valid} "
                              f"trigger={e.record_trigger_index} start={e.window_start}")
                        mismatches += 1
                if block == 128:
                    triggers = [int(g[1]) for g in got]
                    print(f"{name}: {len(got)} window(s), triggers {triggers}  (detector.py: "
                          f"{[e.record_trigger_index for e in ref]})")
        finally:
            os.unlink(csv)
    print(f"compare_detector: {mismatches} mismatch(es)")
    return mismatches


if __name__ == "__main__":
    sys.exit(main())
