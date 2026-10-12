# Distributed Acoustic Localization

ENSC 351 group project.
Several BeagleY-AI "nodes" each estimate the direction of a sound and report it to a "master" BeagleY-AI, which triangulates the location of the source.

## Layout

- `node/`: the executable that runs on each node board
- `master/`: the executable that runs on the master board

## Building

Cross-compile for the BeagleY-AI from the project root:

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=aarch64-toolchain.cmake
cmake --build build
```

Both executables are copied to `~/ensc351/public/myApps/` after a successful build.
Add `-DENABLE_ASAN=ON` to the first command to build with the address sanitizer.
