"""File input/output for two-channel recordings and extracted events."""
import json
import wave
from pathlib import Path

import numpy as np

from detector import EventWindow, event_to_dict


def load_recording(path, fs=None):
    """Load a two-channel recording. Returns (data of shape (2, N), fs).

    .wav  stereo PCM (8/16/32-bit); values are scaled to [-1, 1). Channel 0 = left.
    .npy  array (2, N) or (N, 2); fs must be given.
    .npz  keys 'data' and 'fs' (fs may be overridden by the argument).
    """
    path = Path(path)
    ext = path.suffix.lower()
    if ext == ".wav":
        with wave.open(str(path), "rb") as w:
            if w.getnchannels() != 2:
                raise ValueError(f"{path.name}: need 2 channels, found {w.getnchannels()}")
            width, rate, raw = w.getsampwidth(), w.getframerate(), w.readframes(w.getnframes())
        if width == 1:
            x = (np.frombuffer(raw, np.uint8).astype(float) - 128.0) / 128.0
        elif width == 2:
            x = np.frombuffer(raw, "<i2").astype(float) / 32768.0
        elif width == 4:
            x = np.frombuffer(raw, "<i4").astype(float) / 2147483648.0
        else:
            raise ValueError(f"{path.name}: unsupported sample width {width * 8} bit")
        return x.reshape(-1, 2).T.copy(), float(fs or rate)
    if ext == ".npy":
        if fs is None:
            raise ValueError(".npy files need --fs")
        data = np.load(path)
        data = data.T if (data.ndim == 2 and data.shape[0] != 2 and data.shape[1] == 2) else data
        return np.asarray(data, float), float(fs)
    if ext == ".npz":
        z = np.load(path)
        rate = fs if fs is not None else float(z["fs"])
        return np.asarray(z["data"], float), float(rate)
    raise ValueError(f"unsupported file type: {ext}")


def write_wav(path, data, fs, gain=0.5):
    """Write (2, N) float data as 16-bit stereo WAV, multiplied by `gain` first."""
    x = np.clip(np.asarray(data) * gain, -1.0, 32767 / 32768)
    pcm = np.round(x.T * 32768.0).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(int(round(fs)))
        w.writeframes(pcm.tobytes())


def save_event(event: EventWindow, prefix) -> tuple:
    """Write <prefix>.npz (arrays) and <prefix>.json (metadata). Returns both paths."""
    if not event.valid:
        raise ValueError("cannot save an invalid event: " + event.status)
    prefix = Path(prefix)
    npz, js = prefix.with_suffix(".npz"), prefix.with_suffix(".json")
    np.savez(npz, samples_a=event.samples_a, samples_b=event.samples_b,
             raw_a=event.raw_a, raw_b=event.raw_b)
    js.write_text(json.dumps(event_to_dict(event), indent=2, default=_jsonable))
    return npz, js


def _jsonable(o):
    if isinstance(o, np.generic):
        return o.item()
    if isinstance(o, np.ndarray):
        return o.tolist()
    raise TypeError(f"not JSON serialisable: {type(o)}")
