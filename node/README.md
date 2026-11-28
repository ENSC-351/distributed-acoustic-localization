# node

Runs on each node board: samples the two mics, finds impacts, estimates
their direction and sends it to the master.

```
microphones -> AudioCapture (R5 or Linux sampler) -> detector -> TDOA estimator -> event queue -> master
                audio/                               dsp/detector.h  dsp/tdoa.h   src/dspLink.c   src/netClient.c
```

```
sudo DSP_MIC_SPACING_M=0.20 ./node <node ID> <master IP> [port]
```

`sudo` is needed for the R5 sampler (it reads the R5's shared memory through
`/dev/mem`). Start the R5 first (`r5capture/board/r5_start.sh`), or the node
uses the Linux sampler, which needs its own setup (`audio/README.md`). If the
capture stops, the node keeps running (heartbeats say the DSP is dead) and
retries every second.

## Settings (environment variables)

| Variable | Default | Meaning |
|---|---|---|
| `DSP_MIC_SPACING_M` | 0.20 | Distance between the mics, metres. **Measure it**; angles scale with it |
| `DSP_SWAP_MICS` | 0 | 1 = mic A is ADC channel 1. Use if theta comes out with the wrong sign |
| `DSP_METHOD` | xcorr | `xcorr` or `phat` (GCC-PHAT) |
| `DSP_BACKEND` | auto | `auto`, `r5` or `linux` |
| `DSP_REPLAY` | | Play a recording (`a,b` per line, ADC counts) instead of the mics, looped in real time |
| `DSP_REPLAY_FS` | 20000 | Sample rate of the replay file |
| `DSP_VERBOSE` | 0 | 1 = print every window and estimate |

Theta convention (`dsp/README.md`): 0 is broadside, positive toward mic B
(ADC channel 1 unless swapped). Two mics can't tell front from back, so every
event has `PROTO_EVENT_THETA_MIRROR`.

Every 10 s the node prints a summary: blocks processed, windows detected,
directions sent, refusals by reason (low energy, weak peak, out of range, ...)
and events dropped.

## Testing without hardware

```
python3 dsp/tools/make_replay.py replay.csv      # synthetic bursts with known delays; prints the expected angles
./master 47000 &
DSP_REPLAY=replay.csv DSP_VERBOSE=1 ./node 1 127.0.0.1 47000
```

Build with `-DNODE_FAKE_DSP=ON` for the old made-up events (networking tests only).

**Status:** the detector and estimator have only seen synthetic data. Real
taps (and the detector's thresholds) are untested.
