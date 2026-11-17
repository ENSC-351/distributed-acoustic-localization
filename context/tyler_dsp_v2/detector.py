"""Impact detection and aligned window extraction for a two-microphone node.

Pure functions, no file or network I/O (see io_utils.py for that), so this can
sit between the sampler and the TDOA estimator and later be ported to C.

Rules this module follows (from the DSP roadmap):
  * The trigger comes from BOTH channels combined; the SAME sample indices are
    cut from both channels, so the inter-channel delay is preserved.
  * Neither channel is ever shifted. Channel skew is passed through and
    reported, with skew_correction_applied_s = 0.
  * Raw samples are always kept next to the DC-removed samples.
  * DC removal for the hand-off happens in exactly one place: per window, from
    the pre-trigger samples. (Detection uses its own internal DC estimate; it
    never touches the output samples.)
"""
from dataclasses import asdict, dataclass, field, replace
from typing import List, Optional, Tuple

import numpy as np

SPEED_OF_SOUND_MPS = 343.0   # assumed room-temperature value; recorded in output


def max_delay_samples(mic_spacing_m: float, fs: float,
                      c: float = SPEED_OF_SOUND_MPS) -> float:
    """Largest physically possible inter-mic delay, in samples: fs * d / c."""
    return fs * mic_spacing_m / c


@dataclass
class DetectorConfig:
    threshold_ratio: float = 12.0   # trigger at this multiple of the noise-floor energy
    smooth_ms: float = 0.5          # moving-average length of the energy envelope
    pre_ms: float = 5.0             # kept before the trigger
    post_ms: float = 15.0           # kept after the trigger
    holdoff_ms: float = 50.0        # ignore triggers this long after one fires
    rearm_ratio: float = 0.5        # energy must fall below this * threshold to re-arm
    min_noise_floor: float = 1e-9   # avoids divide-by-zero on perfect silence
    adc_range: Optional[Tuple[float, float]] = None   # e.g. (0, 4095) to flag clipping
    speed_of_sound_mps: float = SPEED_OF_SOUND_MPS


@dataclass
class NoiseModel:
    """Fixed DC and noise floor, e.g. calibrated once from a quiet capture.
    Live use needs this: there is no whole record to take a median of."""
    dc: Tuple[float, float]
    floor_energy: float


@dataclass
class EventWindow:
    """One extracted event. Field names follow the roadmap's EventWindow table."""
    valid: bool
    status: str                         # "ok" or the reason it is invalid
    sample_rate_hz: float               # effective PER-CHANNEL rate
    channel_order: Tuple[str, str]
    samples_a: Optional[np.ndarray] = None   # DC-removed, window-aligned
    samples_b: Optional[np.ndarray] = None
    raw_a: Optional[np.ndarray] = None       # untouched samples, same indices
    raw_b: Optional[np.ndarray] = None
    sample_count: int = 0
    trigger_index: Optional[int] = None          # index WITHIN the window
    record_trigger_index: Optional[int] = None   # index in the original record
    window_start: Optional[int] = None           # in the original record
    window_end: Optional[int] = None             # exclusive
    event_time_ns: Optional[int] = None          # time of window's FIRST sample
    event_time_clock: Optional[str] = None
    mic_spacing_m: Optional[float] = None        # pass-through
    channel_skew_s: Optional[float] = None       # pass-through (B minus A)
    skew_correction_applied_s: float = 0.0       # always 0: never silently corrected
    dc_removed: bool = False                     # True for samples_a / samples_b
    flags: dict = field(default_factory=dict)
    processing: List[str] = field(default_factory=list)
    diagnostics: dict = field(default_factory=dict)


# ---------------------------------------------------------------- helpers

def remove_dc(x: np.ndarray) -> np.ndarray:
    """Subtract each channel's median (robust to the impact itself)."""
    x = np.asarray(x, dtype=float)
    return x - np.median(x, axis=-1, keepdims=True)


def combined_energy(x: np.ndarray, smooth_samples: int) -> np.ndarray:
    """Short-time energy summed over both channels (causal moving average)."""
    power = np.sum(np.square(x), axis=0)
    kernel = np.ones(smooth_samples) / smooth_samples
    energy = np.convolve(power, kernel, mode="full")[: power.size]
    energy[: smooth_samples - 1] = np.nan   # not enough history yet
    return energy


def _as_two_channel(data):
    """Return (array of shape (2, N), None) or (None, error message)."""
    try:
        if isinstance(data, (tuple, list)) and len(data) == 2:
            a, b = np.asarray(data[0], float), np.asarray(data[1], float)
            if a.ndim != 1 or b.ndim != 1:
                return None, "channels must be 1-D"
            if a.size != b.size:
                return None, f"channel length mismatch ({a.size} vs {b.size})"
            arr = np.vstack([a, b])
        else:
            arr = np.asarray(data, dtype=float)
    except (TypeError, ValueError):
        return None, "data is not numeric"
    if arr.ndim != 2 or arr.shape[0] != 2:
        return None, "data must have shape (2, N)"
    if arr.shape[1] == 0:
        return None, "empty input"
    if not np.all(np.isfinite(arr)):
        return None, "non-finite samples (NaN/inf)"
    return arr, None


def calibrate_noise(quiet, fs: float, cfg: Optional[DetectorConfig] = None) -> NoiseModel:
    """Estimate DC and noise-floor energy from a capture with no impacts."""
    cfg = cfg or DetectorConfig()
    arr, err = _as_two_channel(quiet)
    if err:
        raise ValueError(err)
    smooth = max(1, int(round(cfg.smooth_ms * 1e-3 * fs)))
    dc = np.median(arr, axis=1)
    energy = combined_energy(arr - dc[:, None], smooth)
    return NoiseModel((float(dc[0]), float(dc[1])), float(np.nanmedian(energy)))


# ---------------------------------------------------------------- detection

def _run(data, fs, cfg, order, spacing, skew, noise):
    """Returns (events, diagnostics, error_event_or_None)."""
    base = dict(sample_rate_hz=fs, channel_order=tuple(order),
                mic_spacing_m=spacing, channel_skew_s=skew)

    def bad(msg):
        return [], {}, EventWindow(False, "invalid input: " + msg, **base)

    if not isinstance(fs, (int, float)) or not np.isfinite(fs) or fs <= 0:
        return bad("sample rate must be a positive number")
    arr, err = _as_two_channel(data)
    if err:
        return bad(err)
    n = arr.shape[1]
    smooth = max(1, int(round(cfg.smooth_ms * 1e-3 * fs)))
    pre = int(round(cfg.pre_ms * 1e-3 * fs))
    post = int(round(cfg.post_ms * 1e-3 * fs))
    holdoff = int(round(cfg.holdoff_ms * 1e-3 * fs))
    if n < smooth + 1:
        return bad("record shorter than smoothing window")

    # Detection-only conditioning (never applied to the output samples).
    if noise is None:
        det_dc = np.median(arr, axis=1)
        energy = combined_energy(arr - det_dc[:, None], smooth)
        floor = float(np.nanmedian(energy))
        floor_src = "record median"
    else:
        det_dc = np.asarray(noise.dc, dtype=float)
        energy = combined_energy(arr - det_dc[:, None], smooth)
        floor = float(noise.floor_energy)
        floor_src = "calibration"
    floor = max(floor, cfg.min_noise_floor)
    threshold = cfg.threshold_ratio * floor
    e = np.nan_to_num(energy, nan=0.0)

    diag = {
        "noise_floor_energy": floor, "noise_floor_source": floor_src,
        "threshold_energy": threshold,
        "peak_to_floor_ratio": float(np.max(e) / floor),
        "smooth_samples": smooth, "pre_samples": pre, "post_samples": post,
        "holdoff_samples": holdoff,
        "trigger_may_lag_onset_by_up_to_samples": smooth,
        "speed_of_sound_mps": cfg.speed_of_sound_mps,
    }
    if spacing is not None:
        diag["max_delay_samples"] = max_delay_samples(spacing, fs, cfg.speed_of_sound_mps)

    events: List[EventWindow] = []
    pos = 0
    while pos < n:
        hits = np.flatnonzero(e[pos:] > threshold)
        if hits.size == 0:
            break
        trig = pos + int(hits[0])
        events.append(_build_window(arr, fs, cfg, order, spacing, skew, trig,
                                    pre, post, smooth, threshold, diag))
        nxt = trig + holdoff
        if nxt >= n:
            break
        quiet = np.flatnonzero(e[nxt:] <= cfg.rearm_ratio * threshold)
        if quiet.size == 0:
            break
        pos = nxt + int(quiet[0])
    return events, diag, None


def _build_window(arr, fs, cfg, order, spacing, skew, trig, pre, post, smooth,
                  threshold, diag):
    n = arr.shape[1]
    start, end = trig - pre, trig + post
    base = dict(sample_rate_hz=fs, channel_order=tuple(order),
                mic_spacing_m=spacing, channel_skew_s=skew,
                record_trigger_index=trig, diagnostics=dict(diag))
    steps = [f"detection_dc_removal(median, internal only)",
             f"energy_trigger(ratio={cfg.threshold_ratio}, smooth={smooth} samples)"]
    if start < 0 or end > n:
        return EventWindow(False, "window extends past record edge",
                           flags={"incomplete": True}, processing=steps, **base)

    raw = arr[:, start:end].copy()                 # same indices on both channels
    tw = trig - start
    ref = raw[:, :tw] if tw >= 8 else raw          # pre-trigger samples = no impact yet
    win_dc = np.median(ref, axis=1)
    proc = raw - win_dc[:, None]
    steps += [f"window(start={start}, end={end}, same indices both channels)",
              "window_dc_removal(median of pre-trigger samples, per channel)"
              if tw >= 8 else "window_dc_removal(median of whole window; pre-trigger too short)"]

    flags = {"incomplete": False, "clipped_a": None, "clipped_b": None}
    if cfg.adc_range is not None:
        lo, hi = cfg.adc_range
        flags["clipped_a"] = bool(np.any((raw[0] <= lo) | (raw[0] >= hi)))
        flags["clipped_b"] = bool(np.any((raw[1] <= lo) | (raw[1] >= hi)))
    base["diagnostics"]["window_dc_estimate"] = [float(win_dc[0]), float(win_dc[1])]

    return EventWindow(True, "ok", samples_a=proc[0], samples_b=proc[1],
                       raw_a=raw[0], raw_b=raw[1], sample_count=end - start,
                       trigger_index=tw, window_start=start, window_end=end,
                       dc_removed=True, flags=flags, processing=steps, **base)


def detect_events(data, fs, cfg: Optional[DetectorConfig] = None,
                  channel_order=("mic0", "mic1"), mic_spacing_m=None,
                  channel_skew_s=None, noise_model: Optional[NoiseModel] = None
                  ) -> List[EventWindow]:
    """All events in a record, separated by holdoff + re-arm.

    data: array (2, N) or a pair (a, b). Returns [] if nothing triggered, or a
    single invalid EventWindow if the input is bad.
    """
    cfg = cfg or DetectorConfig()
    events, _, err = _run(data, fs, cfg, channel_order, mic_spacing_m,
                          channel_skew_s, noise_model)
    return [err] if err else events


def detect_impact(data, fs, cfg: Optional[DetectorConfig] = None,
                  channel_order=("mic0", "mic1"), mic_spacing_m=None,
                  channel_skew_s=None, noise_model: Optional[NoiseModel] = None
                  ) -> EventWindow:
    """First event in the record, or an invalid result saying why not."""
    cfg = cfg or DetectorConfig()
    events, diag, err = _run(data, fs, cfg, channel_order, mic_spacing_m,
                             channel_skew_s, noise_model)
    if err:
        return err
    if events:
        return events[0]
    return EventWindow(False, "no impact detected", sample_rate_hz=fs,
                       channel_order=tuple(channel_order), mic_spacing_m=mic_spacing_m,
                       channel_skew_s=channel_skew_s, diagnostics=diag)


# ---------------------------------------------------------------- hand-off

def with_buffer_start_time(event: EventWindow, buffer_start_ns: int,
                           clock: str = "CLOCK_REALTIME") -> EventWindow:
    """Attach a time to the event, given when the acquisition buffer started.

    event_time_ns refers to the window's FIRST sample:
        buffer start + window_start / sample_rate
    (not the trigger: the trigger sits slightly after the true onset).
    """
    if not event.valid:
        raise ValueError("cannot timestamp an invalid event")
    offset_ns = int(round(event.window_start / event.sample_rate_hz * 1e9))
    return replace(event, event_time_ns=int(buffer_start_ns) + offset_ns,
                   event_time_clock=clock)


def event_to_dict(event: EventWindow) -> dict:
    """JSON-friendly metadata (no sample arrays)."""
    d = asdict(event)
    for k in ("samples_a", "samples_b", "raw_a", "raw_b"):
        d.pop(k)
    return d
