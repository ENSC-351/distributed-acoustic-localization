"""Tests for detection and window extraction. Run from dsp/:  python3 -m pytest -v"""
import json

import numpy as np
import pytest

import run_detector
from detector import (DetectorConfig, calibrate_noise, detect_events, detect_impact,
                      max_delay_samples, remove_dc, with_buffer_start_time)
from io_utils import load_recording, save_event, write_wav
from make_testset import generate
from synth import make_record

FS = 48000.0


def lag_of(a, b):
    """Samples by which b lags a (positive = b later)."""
    corr = np.correlate(b, a, mode="full")
    return int(np.argmax(corr)) - (len(a) - 1)


# ---- delay sign and alignment (roadmap: "Delay sign", "Zero delay") ----

@pytest.mark.parametrize("delay", [0, 3, 7, -5])
def test_known_delay_preserved_in_window(delay):
    data = make_record(fs=FS, delay_samples=delay)
    ev = detect_impact(data, FS)
    assert ev.valid, ev.status
    assert np.array_equal(ev.raw_a, data[0, ev.window_start:ev.window_end])
    assert np.array_equal(ev.raw_b, data[1, ev.window_start:ev.window_end])
    assert lag_of(ev.samples_a, ev.samples_b) == delay


def test_channel_swap_reverses_delay_sign():
    data = make_record(fs=FS, delay_samples=7)
    ev = detect_impact(data, FS)
    sw = detect_impact(data[::-1], FS)
    assert lag_of(ev.samples_a, ev.samples_b) == 7
    assert lag_of(sw.samples_a, sw.samples_b) == -7


def test_window_geometry_and_trigger_index():
    ev = detect_impact(make_record(fs=FS), FS, DetectorConfig(pre_ms=5, post_ms=15))
    assert ev.trigger_index == int(round(0.005 * FS))        # index within the window
    assert ev.sample_count == int(round(0.020 * FS)) == len(ev.samples_a) == len(ev.samples_b)
    assert ev.record_trigger_index - ev.window_start == ev.trigger_index


def test_trigger_near_true_onset():
    ev = detect_impact(make_record(fs=FS, impact_time_s=0.08), FS)
    true_idx = int(0.08 * FS)
    assert true_idx <= ev.record_trigger_index <= true_idx + ev.diagnostics["smooth_samples"] + 2


# ---- no event / noise (roadmap: "No event", "Noisy impact") ----

def test_no_impact_gives_invalid_result_and_no_events():
    data = make_record(fs=FS, impact=False)
    ev = detect_impact(data, FS)
    assert not ev.valid and ev.status == "no impact detected" and ev.samples_a is None
    assert detect_events(data, FS) == []


def test_perfect_silence_no_nan_no_crash():
    ev = detect_impact(np.zeros((2, 5000)), FS)
    assert not ev.valid and ev.status == "no impact detected"


def test_low_snr_impact_still_detected():
    data = make_record(fs=FS, amplitude=0.4, noise_std=0.05)
    assert detect_impact(data, FS).valid


# ---- DC handling: exactly one stage, raw kept ----

def test_window_dc_from_pre_trigger_and_raw_untouched():
    ev = detect_impact(make_record(fs=FS, dc_offsets=(0.3, -0.1)), FS)
    pre = ev.trigger_index
    assert ev.dc_removed
    assert abs(np.median(ev.samples_a[:pre])) < 1e-12
    assert abs(np.median(ev.samples_b[:pre])) < 1e-12
    assert np.median(ev.raw_a[:pre]) > 0.25                 # raw still has the offset
    assert any("window_dc_removal" in p for p in ev.processing)


# ---- holdoff / re-arm (multiple events) ----

def test_two_separated_impacts_give_two_events():
    data = make_record(fs=FS, impact_time_s=[0.05, 0.14], duration_s=0.25)
    events = detect_events(data, FS)
    assert [e.valid for e in events] == [True, True]
    assert abs(events[1].record_trigger_index - int(0.14 * FS)) < 60


def test_second_impact_inside_holdoff_is_suppressed():
    data = make_record(fs=FS, impact_time_s=[0.05, 0.07], duration_s=0.25)
    assert len(detect_events(data, FS, DetectorConfig(holdoff_ms=50))) == 1
    assert len(detect_events(data, FS, DetectorConfig(holdoff_ms=5))) == 2


def test_one_impact_with_long_ringdown_triggers_once():
    data = make_record(fs=FS, decay=150.0, duration_s=0.3)
    assert len(detect_events(data, FS)) == 1


# ---- flags, pass-through, hand-off ----

def test_clipping_flag():
    data = np.clip(make_record(fs=FS, amplitude=2.0), -0.5, 0.9)
    ev = detect_impact(data, FS, DetectorConfig(adc_range=(-0.5, 0.9)))
    assert ev.valid and ev.flags["clipped_a"] and ev.flags["clipped_b"]
    ok = detect_impact(make_record(fs=FS), FS, DetectorConfig(adc_range=(-5, 5)))
    assert ok.flags["clipped_a"] is False
    assert detect_impact(make_record(fs=FS), FS).flags["clipped_a"] is None   # unknown


def test_skew_and_spacing_passed_through_never_corrected():
    data = make_record(fs=FS, delay_samples=7)
    ev = detect_impact(data, FS, mic_spacing_m=0.2, channel_skew_s=1e-5)
    assert ev.mic_spacing_m == 0.2 and ev.channel_skew_s == 1e-5
    assert ev.skew_correction_applied_s == 0.0
    assert lag_of(ev.samples_a, ev.samples_b) == 7           # nothing was shifted
    assert ev.diagnostics["max_delay_samples"] == pytest.approx(FS * 0.2 / 343.0)


def test_max_delay_samples_formula():
    assert max_delay_samples(0.343, 1000.0) == pytest.approx(1.0)


def test_buffer_start_time_applied_to_first_sample_of_window():
    ev = detect_impact(make_record(fs=FS), FS)
    t = with_buffer_start_time(ev, 1_000_000_000)
    assert t.event_time_clock == "CLOCK_REALTIME"
    assert t.event_time_ns == 1_000_000_000 + round(ev.window_start / FS * 1e9)
    with pytest.raises(ValueError):
        with_buffer_start_time(detect_impact(np.zeros((2, 5000)), FS), 0)


def test_calibrated_noise_model_detects_same_event():
    quiet = make_record(fs=FS, impact=False, seed=5)
    nm = calibrate_noise(quiet, FS)
    data = make_record(fs=FS, seed=6)
    a, b = detect_impact(data, FS), detect_impact(data, FS, noise_model=nm)
    assert b.valid and abs(a.record_trigger_index - b.record_trigger_index) <= 2
    assert b.diagnostics["noise_floor_source"] == "calibration"


# ---- bad input (roadmap: "Bad input") ----

@pytest.mark.parametrize("bad, text", [
    (np.zeros((3, 1000)), "shape"),
    (np.zeros((2, 0)), "empty"),
    ((np.zeros(100), np.zeros(90)), "mismatch"),
    (np.array([[0.0, np.nan] * 500, [0.0, 1.0] * 500]), "non-finite"),
    (np.array([[0.0, np.inf] * 500, [0.0, 1.0] * 500]), "non-finite"),
    ("not data", "numeric"),
])
def test_bad_data_rejected_with_explicit_status(bad, text):
    ev = detect_impact(bad, FS)
    assert not ev.valid and ev.status.startswith("invalid input") and text in ev.status
    assert ev.samples_a is None


@pytest.mark.parametrize("fs", [0, -1, float("nan")])
def test_bad_sample_rate_rejected(fs):
    ev = detect_impact(make_record(), fs)
    assert not ev.valid and "sample rate" in ev.status


def test_impact_too_close_to_edge_is_invalid_and_incomplete():
    ev = detect_impact(make_record(fs=FS, impact_time_s=0.001), FS)
    assert not ev.valid and "edge" in ev.status and ev.flags["incomplete"]


def test_pair_input_equals_array_input():
    d = make_record(fs=FS)
    a, b = detect_impact(d, FS), detect_impact((d[0], d[1]), FS)
    assert np.array_equal(a.raw_a, b.raw_a)


def test_remove_dc():
    assert np.allclose(remove_dc(np.array([[1.0, 1, 1], [5, 5, 5]])), 0.0)


# ---- file I/O and command line ----

def test_wav_roundtrip_and_detection(tmp_path):
    data = make_record(fs=FS, delay_samples=7)
    write_wav(tmp_path / "a.wav", data, FS, gain=0.5)
    back, fs = load_recording(tmp_path / "a.wav")
    assert fs == FS and back.shape == data.shape
    assert np.allclose(back, data * 0.5, atol=1 / 32768)
    ev = detect_impact(back, fs)
    assert ev.valid and lag_of(ev.samples_a, ev.samples_b) == 7


def test_mono_wav_rejected(tmp_path):
    import wave
    with wave.open(str(tmp_path / "m.wav"), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000)
        w.writeframes(b"\x00\x00" * 100)
    with pytest.raises(ValueError):
        load_recording(tmp_path / "m.wav")


def test_npy_needs_fs(tmp_path):
    np.save(tmp_path / "x.npy", make_record(fs=FS))
    with pytest.raises(ValueError):
        load_recording(tmp_path / "x.npy")
    data, fs = load_recording(tmp_path / "x.npy", fs=FS)
    assert fs == FS and data.shape[0] == 2


def test_save_event_writes_arrays_and_metadata(tmp_path):
    ev = detect_impact(make_record(fs=FS), FS, mic_spacing_m=0.2)
    npz, js = save_event(ev, tmp_path / "e")
    z = np.load(npz)
    assert np.array_equal(z["samples_a"], ev.samples_a) and np.array_equal(z["raw_b"], ev.raw_b)
    meta = json.loads(js.read_text())
    assert meta["sample_rate_hz"] == FS and meta["trigger_index"] == ev.trigger_index
    assert meta["skew_correction_applied_s"] == 0.0 and "samples_a" not in meta
    with pytest.raises(ValueError):
        save_event(detect_impact(np.zeros((2, 5000)), FS), tmp_path / "bad")


def test_testset_matches_manifest_and_cli(tmp_path, capsys):
    manifest = generate(tmp_path)
    for name, info in manifest.items():
        data, fs = load_recording(tmp_path / f"{name}.npz")
        events = [e for e in detect_events(data, fs) if e.valid]
        assert len(events) == info["expected_events"], name
        if info["expected_delay_samples"] is not None and name != "low_snr":
            e = events[0]
            assert abs(lag_of(e.samples_a, e.samples_b) - info["expected_delay_samples"]) <= 0.5, name
    rc = run_detector.main([str(tmp_path / "delay_plus7.wav"), "--out-prefix", str(tmp_path / "o" / "r")])
    assert rc == 0 and (tmp_path / "o" / "r_event0.json").exists()
    assert "trigger @ record index" in capsys.readouterr().out
