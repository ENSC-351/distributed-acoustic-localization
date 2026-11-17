"""Synthetic two-microphone data with known inter-channel delays.

Sign convention (matches the roadmap): delay_samples = t_B - t_A in samples.
Positive means channel B (row 1) hears the impact LATER than channel A (row 0).
"""
import numpy as np


def _burst(t: np.ndarray, f0: float, decay: float) -> np.ndarray:
    """Damped sinusoid starting at t = 0 (an idealised impact 'knock')."""
    out = np.zeros_like(t)
    m = t >= 0
    out[m] = np.exp(-decay * t[m]) * np.sin(2 * np.pi * f0 * t[m])
    return out


def make_record(
    fs: float = 48000.0,
    duration_s: float = 0.2,
    impact_time_s=0.08,             # a float, or a list for several impacts
    delay_samples: float = 7.0,     # may be negative or fractional
    amplitude: float = 1.0,
    noise_std: float = 0.02,
    dc_offsets=(0.3, -0.1),
    f0: float = 1500.0,
    decay: float = 800.0,
    impact: bool = True,
    seed: int = 0,
) -> np.ndarray:
    """Return an array of shape (2, N): row 0 = mic A, row 1 = mic B."""
    rng = np.random.default_rng(seed)
    n = int(round(duration_s * fs))
    t = np.arange(n) / fs
    data = rng.normal(0.0, noise_std, size=(2, n))
    if impact:
        for t0 in np.atleast_1d(impact_time_s):
            data[0] += amplitude * _burst(t - t0, f0, decay)
            data[1] += amplitude * _burst(t - t0 - delay_samples / fs, f0, decay)
    data[0] += dc_offsets[0]
    data[1] += dc_offsets[1]
    return data
