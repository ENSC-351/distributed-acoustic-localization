# Distributed Acoustic Localization

ENSC 351 group project.
Several BeagleY-AI "nodes" each estimate the direction of a sound and report it to a "master" BeagleY-AI, which triangulates the location of the source.

## Layout

- `node/`: the executable that runs on each node board
- `master/`: the executable that runs on the master board. Groups the nodes' direction reports for each sound and triangulates it (least-squares crossing of the lines). Node positions are in `master/src/layout.c` (equilateral triangle, every node facing the centre). Serves its state to the web GUI over UDP port 12345. `master --demo` makes fake sounds that go through the real triangulation, until node networking exists
- `web/`: Node.js web GUI for the master (port 8088): the node triangle, each node's direction line for the latest sound, a heat map of where the lines agree, and a pulse at the estimate. Relays to the master over UDP

## Building

Cross-compile for the BeagleY-AI from the project root:

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=aarch64-toolchain.cmake
cmake --build build
```

Both executables are copied to `~/ensc351/public/myApps/` after a successful build.
Add `-DENABLE_ASAN=ON` to the first command to build with the address sanitizer.

## Web GUI

Full commands (build, copy to the board, run, settings): `web/RUN.md`. In short, on the master board:

```
./master --demo
cd web && node server.js
```

Then browse to `http://<board IP>:8088`.

