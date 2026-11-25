# Distributed Acoustic Localization

ENSC 351 group project.
Several BeagleY-AI "nodes" each estimate the direction of a sound and report it to a "master" BeagleY-AI, which triangulates the location of the source.

## Layout

- `node/`: the executable that runs on each node board
- `master/`: the executable that runs on the master board
- `common/`: library shared by the node and master (protocol)
- `hal/`: hardware abstraction library (MCP3208 ADC over SPI)
- `audio/`: samples the two microphones (ADC channels 0 and 1) into timestamped blocks for signal analysis. Currently the **Linux reserved-core version** (see `audio/CONTEXT.md`)
- `r5capture/`: work in progress to sample on the R5F core instead (start with `r5capture/CONTEXT.md`)
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
