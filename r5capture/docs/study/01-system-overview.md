# 1. System overview

## What we're building

Several **nodes** (BeagleY-AI boards), each with two microphones on an MCP3208 ADC,
detect an impact sound (e.g. a marble landing) and estimate the **direction** it came
from. Each node sends that direction over TCP to a **master** BeagleY-AI, which will
combine directions from several nodes to find the **position**, and show it in a GUI.

Why directions and not arrival times across nodes? Cross-node timing would need the
boards' clocks synced to far better than a millisecond. Clock sync (chrony) is about
1 ms, and sound moves ~34 cm in 1 ms, which is useless for position. Each node instead
measures a *local* delay between its own two mics (sampled by the same clock), turns it
into an angle, and the master intersects the angles (bearings).

## Data flow on one node

```
 mic A ─┐ preamp ┐
        │        ├─► MCP3208 ADC ─SPI─► sampler ─► detector ─► estimator ─► networking ─► master
 mic B ─┘ preamp ┘   (8 ch, 12-bit)     (Joey)     (Tyler)     (Madden)     (Jack)       (Zur/Jack)
```

| Stage | What it does | Status (2026-11-25) |
|---|---|---|
| Preamp | Electret mic → op-amp gain (~13×, *estimate*) → re-biased to mid-supply for the ADC. No anti-aliasing filter yet | Built |
| Sampler | Reads both mic channels continuously into timestamped blocks | Linux version works (Joey); R5 version in progress (this repo) |
| Detector | Finds impacts and cuts an aligned two-channel window around each | Python prototype (Tyler), 36 tests |
| Estimator | Finds the delay between the mics in a window → angle, or refuses | C library (Madden), 760 checks |
| Networking | Node connects, HELLO, heartbeats every 1 s, sends EVENTs | Works (Jack) |
| Master | Accepts nodes, logs events, event buffer; fusion not yet | Networking works (Zur, Jack) |

## The EVENT message (node → master)

40 bytes. The fields that matter for understanding:

| Field | Meaning |
|---|---|
| `t_event_ns` | CLOCK_REALTIME of the **first sample of the window** (Tyler's convention, adopted) |
| `t_send_ns` | When sent; master can compute network latency |
| `theta` | Degrees, −90 to +90, 0 = broadside (straight out from between the mics), positive toward mic B |
| `phi` | Unused with 2 mics |
| `confidence` | 0 to 1, a heuristic quality score, **not** a probability |
| `dominant_freq_hz` | For matching the same impact across nodes; 0 = not implemented yet |
| `flags` | `THETA_MIRROR` always set: with 2 mics, front and back give the same delay |
| `event_id` | Counter; a gap means events were lost |

Only **valid** estimates are sent (the message has no status field).

## Milestone 2 result (2026-11-16)

On one board, over loopback: Tyler's synthetic windows → estimator → Jack's node →
the C master. All 9 events arrived with the exact angles sent; clean cases within
0.3° of truth; the low-SNR case was off by 4.1° but flagged AMBIGUOUS with confidence
0.02. Estimate time ~7 ms per window on the board (vs 0.8 ms on the x86 VM).

## Why the R5 project exists

The sampler is the stage most affected by Linux: timing between samples depends on
software and the SPI driver. The BeagleY-AI has spare real-time cores (R5F). Moving just
the sampling there gives evenly spaced, gap-free samples regardless of what Linux is
doing. Files 3 to 6 explain why and how.

## Who does what

Madden: estimator, architecture, integration, R5 sampling. Jack: protocol and node
networking. Zur: master networking. Joey (JKolsteren): ADC driver, audio capture library,
audio meter. Tyler: detector, window extraction, test recordings.
