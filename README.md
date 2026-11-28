# Distributed Acoustic Localization

ENSC 351 group project.
Several BeagleY-AI "nodes" each estimate the direction of a sound and report it to a "master" BeagleY-AI, which triangulates the location of the source.

## Layout

- `node/`: the executable that runs on each node board
- `master/`: the executable that runs on the master board. Groups the nodes' direction reports for each sound and triangulates it (least-squares crossing of the lines). Node positions are in `master/src/layout.c` (equilateral triangle, every node facing the centre). Serves its state to the web GUI over UDP port 12345. Nodes connect over TCP (port from `common/protocol.h`); `master --demo` adds fake sounds that go through the same triangulation
- `web/`: Node.js web GUI for the master (port 8088): the node triangle, each node's direction line for the latest sound, a heat map of where the lines agree, and a pulse at the estimate. Relays to the master over UDP
- `common/`: library shared by the node and master (wire protocol)
- `dsp/`: TDOA / direction estimator library and its host tests (see `dsp/README.md`)
- `hal/`: hardware abstraction library (MCP3208 ADC over SPI)
- `audio/`: samples the two microphones (ADC channels 0 and 1) into timestamped blocks for signal analysis. Currently the **Linux reserved-core version** (see `audio/CONTEXT.md`)
- `r5capture/`: work in progress to sample on the R5F core instead (start with `r5capture/SAMPLING_CONTEXT.md`)
- `overlays/`: board setup that `audio/` needs: hardware SPI and a reserved CPU core
- `audio_meter/`: microphone test: live loudness bars for both mics, and a benchmark of sample rate against SPI clock (see `audio_meter/README.md`)

## Building

Cross-compile for the BeagleY-AI from the project root:

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=aarch64-toolchain.cmake
cmake --build build
```

All executables are copied to `~/ensc351/public/myApps/` after a successful build.
Add `-DENABLE_ASAN=ON` to the first command to build with the address sanitizer.

## Web GUI

Full commands (build, copy to the board, run, settings): `web/RUN.md`. In short, on the master board:

```
./master --demo
cd web && node server.js
```

Then browse to `http://<board IP>:8088`.

