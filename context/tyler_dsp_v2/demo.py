"""Demo: detect an impact in synthetic data, print a summary, save two plots.

Usage (from dsp/):  python3 demo.py
Outputs: detection_plot.png (whole record), window_plot.png (extracted window)
"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

from detector import combined_energy, detect_impact, with_buffer_start_time
from synth import make_record

FS = 48000.0
DELAY = 7
data = make_record(fs=FS, delay_samples=DELAY)
ev = detect_impact(data, FS, channel_order=("mic0", "mic1"))
ev = with_buffer_start_time(ev, 1_700_000_000_000_000_000)   # pretend buffer start time

print("valid / status:        ", ev.valid, "/", ev.status)
print("sample rate:           ", ev.sample_rate_hz, "Hz (per channel)")
print("channel order (A, B):  ", ev.channel_order)
print("record trigger index:  ", ev.record_trigger_index, f"(true onset {int(0.08 * FS)})")
print("window in record:      ", ev.window_start, "to", ev.window_end, f"({ev.sample_count} samples)")
print("trigger within window: ", ev.trigger_index)
print("event_time_ns / clock: ", ev.event_time_ns, ev.event_time_clock, "(first sample of window)")
print("channel skew / applied:", ev.channel_skew_s, "/", ev.skew_correction_applied_s)
print("mic spacing:           ", ev.mic_spacing_m, "(not yet measured)")
print("flags:                 ", ev.flags)
print("processing:")
for p in ev.processing:
    print("   -", p)
d = ev.diagnostics
print("diagnostics:            noise floor %.4g, threshold %.4g, peak/floor %.0f"
      % (d["noise_floor_energy"], d["threshold_energy"], d["peak_to_floor_ratio"]))
lag = int(np.argmax(np.correlate(ev.samples_b, ev.samples_a, "full"))) - (ev.sample_count - 1)
print(f"check: channel lag in extracted window = {lag} samples (true delay B-A = {DELAY})")

work = data - np.median(data, axis=1, keepdims=True)
energy = combined_energy(work, d["smooth_samples"])
t = np.arange(data.shape[1]) / FS * 1000
fig, ax = plt.subplots(2, 1, figsize=(10, 6), sharex=True)
ax[0].plot(t, data[0], lw=0.8, label="mic0 / A (raw)")
ax[0].plot(t, data[1], lw=0.8, label="mic1 / B (raw)")
ax[0].axvspan(ev.window_start / FS * 1000, ev.window_end / FS * 1000, color="green", alpha=0.15,
              label="extracted window")
ax[0].set_ylabel("amplitude"); ax[0].legend(loc="upper right")
ax[1].plot(t, energy, "k", lw=0.8, label="combined energy")
ax[1].axhline(d["threshold_energy"], color="r", ls="--", label="threshold")
ax[1].axvline(ev.record_trigger_index / FS * 1000, color="orange", label="trigger")
ax[1].set_xlabel("time (ms)"); ax[1].set_ylabel("energy"); ax[1].legend(loc="upper right")
fig.suptitle("Impact detection on synthetic two-channel record"); fig.tight_layout()
fig.savefig("detection_plot.png", dpi=130)

tw = (np.arange(ev.sample_count) - ev.trigger_index) / FS * 1000
fig, ax = plt.subplots(figsize=(10, 4))
ax.plot(tw, ev.samples_a, label="A (DC removed)", lw=1)
ax.plot(tw, ev.samples_b, label=f"B (DC removed), {DELAY} samples later", lw=1)
ax.axvline(0, color="k", ls="--", label="trigger"); ax.set_xlim(-1, 5)
ax.set_xlabel("time relative to trigger (ms)"); ax.set_ylabel("amplitude")
ax.set_title("Extracted window: both channels cut at the same indices")
ax.legend(); fig.tight_layout(); fig.savefig("window_plot.png", dpi=130)
print("saved detection_plot.png and window_plot.png")
