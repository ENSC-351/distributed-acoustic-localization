# Handoff: complete project status (for a fresh agent or teammate)

Last updated 2026-11-28, on branch `madden/node-integration`. Start here; the linked files
have the details. Everything marked *measured* was run; everything else is design or
synthetic. **Say which in anything you report.**

## 1. The project in one paragraph

ENSC 351 group project: distributed acoustic localization on BeagleY-AI boards (TI AM67A /
J722S). Three **nodes**, each with two microphones on an MCP3208 ADC, sit at the corners of a
triangle. When something hits the table (a marble, a tap), each node estimates the sound's
**direction** (theta) from the time difference between its two mics (TDOA) and sends it over
TCP to a **master**, which intersects the three bearings to locate the impact and shows it
in a web GUI.

Team: Madden (R5 sampling, node integration, master fusion/GUI), Joey (audio capture, R5
sampler, hardware; GitHub JKolsteren), Tyler (impact detector, Python), the DSP estimator
author (GitHub mwbiz26, `dsp/tdoa.c`), Jack (node networking, protocol; jah28), Zur (master
networking, EventBuffer, localizer; zta32).

## 2. Data flow and where each piece lives

```
NODE (one per board)
  R5F core (Zephyr firmware)  ── samples both mics at 20,000 frames/s, timer-paced, interrupts off
     │ shared-memory ring @ 0xA1F00000 (seqlock, 128-frame blocks, R5 timer tick per block)
  Linux: AudioCapture (R5 backend)  audio/src/audio_capture_r5.c, r5_clock.c (ticks -> CLOCK_REALTIME)
     │ blocks
  dspLink.c DSP thread         node/src/dspLink.c
     500 Hz high-pass          dsp/src/highpass.c
     impact detector           dsp/src/detector.c   (streaming C port of Tyler's detector.py)
     TDOA estimator            dsp/src/tdoa.c       (xcorr + parabolic interpolation, skew-corrected)
     event queue (16)  ──DspLink_takeEvent()──▶ netClient.c (Jack) ──TCP EVENT──▶
MASTER
  netServer.c (Jack, on main) ──▶ EventBuffer (Zur) ──▶ Localizer (Zur, IN PROGRESS)
     ──▶ Fusion/Locate/Layout/Dashboard/UDP (madden/feature/web-gui) ──▶ web GUI (Node.js, web/)
```

## 3. Branch and repo state (GitHub: ENSC-351/distributed-acoustic-localization)

| Branch | What | State |
|---|---|---|
| `main` | Jack's protocol, node + master networking, `fakeDsp.c` | Stable |
| **`madden/node-integration`** (this one) | `main` + `dsp/tdoa-estimator` + Joey's `r5-capture` (incl. `audio-meter`) + detector, R5 backend, real `dspLink.c`, tuning, tools, docs | **The next PR into `main`.** Pushed; check `git status` for unpushed commits. Host + aarch64 builds pass, all unit tests pass |
| `zta32/feature/master-networking` | Zur's EventBuffer + `fakeLocalizer.c` skeleton | Builds since Madden's marker fix `eeafa31` (pushed onto it). Localizer being written by Zur |
| `madden/feature/web-gui` | Master fusion (least squares), layout, dashboard, UDP, web GUI, `--demo` | Works with `--demo`; not yet fed by real events |
| `jkolsteren/feature/r5-capture`, `dsp/tdoa-estimator`, `jkolsteren/feature/audio-meter` | Inputs to the integration branch | Fully contained in `madden/node-integration` |
| `madden/r5-capture` | Madden's R5 work before Joey's merge | Superseded by Joey's branch |
| `madden/context-handoff`, `demo/milestone2` | Milestone 2 context and proofs | Historical |

Planned merge order into `main`: **`madden/node-integration`** → Zur's EventBuffer + localizer
→ master `main.c` combining EventBuffer, localizer and the web-gui modules.
Remaining master steps: [master/LOCALIZER_HANDOFF.md](master/LOCALIZER_HANDOFF.md).

## 4. Measured results so far

| What | Result | Source |
|---|---|---|
| R5 sampling | 20,000 frames/s per mic, 0 late frames, 0 ADC errors in every run (also under memory load on Joey's board); 24k clean when quiet | `r5capture/SAMPLING_CONTEXT.md` |
| Channel skew | 17.52 µs from the firmware, 17.45 µs independent check | same |
| Clock | Linux reads the R5 timer directly on Joey's board ("timer clock"); fake-timer tests: median 0.7 µs | `audio/tests` |
| Detector vs Tyler's Python | identical triggers/windows/DC on his 8 recordings at 4 block sizes | `dsp/tools/compare_detector.py` |
| Clap recording (Joey's board, 0.27 m) | Defaults: 216 windows, mostly noise. After tuning: the 9 claps, 0 false, 6 directions, all accepted side claps on the correct side | `r5capture/docs/results/claps_joey_board_2026-11-26.md` |
| Live taps, 0.27 m | 30 windows, 22 sent (73 %), consistent clusters | that doc / conversation |
| Live taps, 0.065 m | 84-91 % accepted; repeated taps within about ±0.25 samples; taps far on the A side clamp at −90° (effective spacing maybe ~6.8 cm) | `dsp/ROADMAP.md` |
| Three-node simulation (synthetic) | theta within ~1°; position error mean 1.5 cm (0.065 m), 0.6 cm (0.27 m) | `dsp/tools/run_triangle_sim.sh` |
| **Angle accuracy at marked positions** | **NOT MEASURED YET**: the next test (section 8) | |

Hardware findings on Joey's board: ch0 is ~10× noisier than ch1 (std 22 vs 2.3 counts);
claps clip in the mic amplifiers (~3340 / ~3630 counts, below the ADC's 4095); no DC drift.

## 5. Node DSP settings (defaults, overridable with environment variables)

Tuned on **one** clap recording (`node/README.md`, top of `node/src/dspLink.c`):
`DSP_MIC_SPACING_M` (0.20 default; **set it**: Joey's board 0.065), `DSP_HIGHPASS_HZ` 500,
`DSP_THRESHOLD` 300 (× noise floor), `DSP_HOLDOFF_MS` 300, `DSP_PRE_MS` 2, `DSP_POST_MS` 5,
`DSP_MIN_PTR` 3 (estimator peak-to-RMS; library default 7 was from synthetic data),
`DSP_METHOD` xcorr, `DSP_BACKEND` auto, `DSP_REPLAY` (file instead of mics), `DSP_VERBOSE`.

Conventions (`dsp/README.md`, pinned by `dsp/tests/test_tdoa.c`): theta 0 = broadside,
positive toward mic B (ADC ch1); `tdoa = lag/fs + channel_skew`; **never shift a channel**,
skew is corrected only in the estimator. Master layout: node 1 top, 2 bottom-left, 3
bottom-right, all facing the centre, **mic B on each node's right**; bearing = heading − theta.

## 6. Environment and how to build, test, run

**Machines.** Development VM (`~/ensc351/work/Project/<worktree>`, git worktrees: `integration`
= this branch, `r5`, `gui`, `handoff`, `dal`). Boards over USB at 192.168.7.2; `~/.ssh/config`
has `myboard` and `joeyboard` (Joey's: user `Joey`). NFS share `~/ensc351/public/myApps` on
the VM = `/mnt/remote/myApps` on Madden's board (Joey's board has no share: use `scp`).
The board package lives in `~/ensc351/public/myApps/integration/`.

**Agent limits.** The VM has **no GitHub credentials** (the user pushes) and the agent has
**no ssh access to the boards** (give the user commands; ask for pasted output or copied files).

**Build** (host and ARM; `CMakeLists.txt` hard-codes `DEPLOY_DIR=$HOME/ensc351/public/myApps`
and every build copies binaries there; set `HOME` to a scratch folder while configuring to
avoid overwriting the share):
```
HOME=/tmp/fakehome cmake -S . -B build-host && cmake --build build-host -j8 && ctest --test-dir build-host
HOME=/tmp/fakehome cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=aarch64-toolchain.cmake && cmake --build build-arm -j8
cd r5capture/test && make test          # R5 firmware logic + ring tests (host)
```
Unit tests: `dsp/tests` (estimator; detector + high-pass, 8,880 checks), `audio/tests` (R5
backend with fake ring/timer, 52 checks), `r5capture/test`. Python with numpy:
`~/zephyrproject/.venv/bin/python`.

**R5 firmware** (Zephyr 4.5.0, SDK 1.0.1): `source ~/zephyrproject/.venv/bin/activate`, then
`west build -p -b beagley_ai/j722s/mcu_r5f0_0 -d build/r5app r5capture/zephyr_app -- -DR5CAP_SPI=mcspi`.
Prebuilt: `r5capture/prebuilt/r5capture-mcspi.elf` (Joey's merged build). On Madden's TI 6.1
kernel the R5 can't be stopped: reboot before every firmware load, never write `start` to a
running core. On Joey's 7.2.8 kernel, stop/start works.

**On a board** (from `~/integration` on Joey's board):
```
sudo ./r5_start.sh                                      # load R5 if needed, sample at 20 kHz
sudo DSP_MIC_SPACING_M=0.065 ./node_r5_test.sh taps 40  # node + local master, prints every estimate
sudo ./record_marks.sh                                  # angle test (section 8)
sudo ./record_claps.sh                                  # guided raw recording
```
Root can't write to the NFS share (root_squash) or overwrite others' files in `/tmp`; the
scripts write next to themselves with timestamped names.

**Offline / VM tools** (`dsp/tools/`): `pipeline_cli` (node DSP over a recording with any
settings), `analyze_marks.py` (per-mark accuracy), `run_triangle_sim.sh` (3 replay nodes +
master), `make_replay.py`, `make_triangle_replay.py`, `detector_cli` + `compare_detector.py`,
`r5capture/tools/analyze_recording.py`.

## 7. Known issues and open questions

- Angle accuracy unmeasured (needs marked positions).
- At 6.5 cm, `confidence` is almost always 1.00 (too few lags for a second peak): needs a new definition.
- Parabolic interpolation bias: ±60° reads ~2° short even on clean synthetic data at 6.5 cm.
- Effective spacing on Joey's board may be ~6.8 cm (taps clamp at −90°); calibrate with ±90° taps.
- ch0 noise and amplifier clipping on Joey's board (hardware).
- Nothing starts the R5 or the node at boot.
- `dominant_freq_hz` is always 0; fusion groups by master receive time, not `t_event_ns`.
- R5 timer may run 0.2 % fast (clock fit handles it; unverified against chrony).
- Madden's board: one unexplained freeze during an R5 load; merged firmware untested on its TI 6.1 kernel.

## 8. What's next (in order)

1. **Angle test on Joey's board** (ready): tape at 1 m at −90…+90° in 30° steps, run
   `sudo ./record_marks.sh`, copy `marks_*` back, then
   `python dsp/tools/analyze_marks.py build-host/dsp/pipeline_cli marks_HHMMSS.wav 0.065`.
2. **PR `madden/node-integration` → `main`** (reviewers: Jack for `dspLink.c`, Joey for the R5
   backend, Tyler for the detector).
3. Zur's localizer, then the combined master `main.c`; re-run `run_triangle_sim.sh` and check
   the GUI ([master/LOCALIZER_HANDOFF.md](master/LOCALIZER_HANDOFF.md)).
4. DSP improvements, scored on the angle-test recording: upsampled correlation, per-channel
   detector normalisation, onset-aligned windows ([dsp/ROADMAP.md](dsp/ROADMAP.md)).
5. systemd autostart; physical triangle (measure `LAYOUT_SIDE_M`); three-board test.

## 9. Working rules (agreed with the team lead, Madden)

- Never commit, push or open PRs without being asked. Local branches are fine. The user pushes.
- **No `Co-Authored-By: Claude` or other AI attribution** in commits or PRs.
- Report results honestly, with measured numbers, including failures; say when data is synthetic.
- Short answers with exact commands; walkthroughs for new core code when asked.
- Proof files for course reports: data and logs only, no AI-sounding prose.
- Don't change another member's module (estimator, detector algorithm, networking) without agreeing it with them.

## 10. Map of the documentation

| File | Covers |
|---|---|
| [node/README.md](node/README.md) | Running the node, settings, replay |
| [dsp/README.md](dsp/README.md), [dsp/ROADMAP.md](dsp/ROADMAP.md) | Estimator conventions; post-MVP improvements and evaluation |
| [audio/README.md](audio/README.md), [audio/CONTEXT.md](audio/CONTEXT.md) | AudioCapture API, both backends |
| [r5capture/SAMPLING_CONTEXT.md](r5capture/SAMPLING_CONTEXT.md) | R5 sampler: results, facts, build, run (Joey) |
| [r5capture/CONTEXT.md](r5capture/CONTEXT.md) | R5 history and toolchain setup |
| [r5capture/docs/integration-plan.md](r5capture/docs/integration-plan.md) | Node integration plan and status |
| [r5capture/docs/results/](r5capture/docs/results/) | Measured data (claps, sweeps, profiles) |
| [r5capture/docs/study/](r5capture/docs/study/) | Study guide: theory, debugging journey |
| [master/LOCALIZER_HANDOFF.md](master/LOCALIZER_HANDOFF.md) | Master side: localizer, `main.c`, merge checklist |
