"""Command-line tool: read a two-channel recording, detect impacts, export windows.

Examples (run from the dsp/ folder):
  python3 run_detector.py testdata/delay_plus7.wav --out-prefix out/run1
  python3 run_detector.py rec.npy --fs 4000 --mic-spacing-m 0.15 --adc-min 0 --adc-max 4095
"""
import argparse
from pathlib import Path

from detector import DetectorConfig, detect_events
from io_utils import load_recording, save_event


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("input", help=".wav (stereo), .npy or .npz")
    p.add_argument("--fs", type=float, help="per-channel sample rate (Hz); needed for .npy")
    p.add_argument("--out-prefix", help="write <prefix>_event<N>.npz/.json for each valid event")
    p.add_argument("--channel-order", nargs=2, default=["mic0", "mic1"], metavar=("A", "B"))
    p.add_argument("--threshold-ratio", type=float, default=12.0)
    p.add_argument("--smooth-ms", type=float, default=0.5)
    p.add_argument("--pre-ms", type=float, default=5.0)
    p.add_argument("--post-ms", type=float, default=15.0)
    p.add_argument("--holdoff-ms", type=float, default=50.0)
    p.add_argument("--mic-spacing-m", type=float, help="measured centre-to-centre spacing")
    p.add_argument("--channel-skew-s", type=float, help="measured B-minus-A sampling offset")
    p.add_argument("--adc-min", type=float)
    p.add_argument("--adc-max", type=float)
    a = p.parse_args(argv)

    data, fs = load_recording(a.input, a.fs)
    adc = (a.adc_min, a.adc_max) if a.adc_min is not None and a.adc_max is not None else None
    cfg = DetectorConfig(threshold_ratio=a.threshold_ratio, smooth_ms=a.smooth_ms,
                         pre_ms=a.pre_ms, post_ms=a.post_ms, holdoff_ms=a.holdoff_ms,
                         adc_range=adc)
    events = detect_events(data, fs, cfg, tuple(a.channel_order),
                           a.mic_spacing_m, a.channel_skew_s)

    print(f"input: {a.input}   fs: {fs:g} Hz   samples/channel: {data.shape[1]}")
    if not events:
        print("no events detected")
        return 0
    for i, ev in enumerate(events):
        if not ev.valid:
            print(f"event {i}: INVALID ({ev.status})  record index {ev.record_trigger_index}")
            continue
        print(f"event {i}: trigger @ record index {ev.record_trigger_index}, "
              f"window [{ev.window_start}, {ev.window_end}) = {ev.sample_count} samples, "
              f"trigger in window = {ev.trigger_index}, flags = {ev.flags}")
        if a.out_prefix:
            Path(a.out_prefix).parent.mkdir(parents=True, exist_ok=True)
            npz, js = save_event(ev, f"{a.out_prefix}_event{i}")
            print(f"  wrote {npz} and {js}")
    if events[0].diagnostics.get("max_delay_samples") is not None:
        print(f"max physical delay: {events[0].diagnostics['max_delay_samples']:.2f} samples")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
