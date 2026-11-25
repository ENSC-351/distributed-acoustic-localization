# Running the master and web GUI

Two programs run on the master board:

- `master`: groups the nodes' direction reports, triangulates, and answers the GUI on UDP port 12345
- `web/server.js`: Node.js web server on port 8088 that relays the browser to `master`

Until node networking exists, run `master --demo`: it makes up sounds and feeds them through the real triangulation.

## 1. Build (VM)

```bash
cd ~/ensc351/work/Project/gui
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=aarch64-toolchain.cmake
cmake --build build                       # copies master to ~/ensc351/public/myApps/
```

## 2. Copy the web server to the NFS share (VM)

```bash
cd ~/ensc351/work/Project/gui/web
npm install                               # first time only; makes node_modules/
mkdir -p ~/ensc351/public/myApps/dal-web
cp -r server.js package.json lib public node_modules ~/ensc351/public/myApps/dal-web/
```

`node_modules` is plain JavaScript, so the copy from the VM works on the board (the board has no internet by default).

## 3. Run (board)

```bash
ssh madden@192.168.7.2
./mountNFS.sh                             # once per boot
/mnt/remote/myApps/master --demo &
cd /mnt/remote/myApps/dal-web && node server.js
```

Open `http://192.168.7.2:8088` in a browser on the host.

## Stopping

- In the page: **Terminate master** (stops `master`; the page then shows "no master")
- Or `Ctrl+C` / `kill %1` for `master`, and `Ctrl+C` for `node server.js`

## Run on the VM instead (no board)

Use a separate build folder so the ARM binary in `myApps` isn't replaced:

```bash
cd ~/ensc351/work/Project/gui
cmake -S . -B build-vm && cmake --build build-vm --target master
./build-vm/master/master --demo &
cd web && npm install && node server.js   # open http://localhost:8088
```

Note: the build's copy step still puts this x86 `master` in `myApps`; rerun `cmake --build build` afterwards before using the board.

## Checking the master without the GUI

```bash
printf status | nc -u -w1 127.0.0.1 12345   # JSON: nodes and recent fixes
printf sound  | nc -u -w1 127.0.0.1 12345   # demo mode: fake a sound now
printf help   | nc -u -w1 127.0.0.1 12345
```

## Settings

| What | Where |
|---|---|
| Distance between nodes (default 2.0 m) | `LAYOUT_SIDE_M` in `master/include/layout.h` |
| Which reports count as one sound (150 ms) | `FUSION_WINDOW_MS` in `master/include/fusion.h` |
| Demo sound period (3 s) | `DEMO_PERIOD_MS` in `master/include/demo_source.h` |
| Quality limits (5 / 15 cm, placeholders) | `GOOD_SPREAD_M`, `FAIR_SPREAD_M` in `web/public/javascripts/dal_ui.js` |
| UDP port (12345) | `UDP_PORT` in `master/include/udp.h` and `MASTER_PORT` in `web/lib/dal_server.js` |
| Web port (8088) | `PORT_NUMBER` in `web/server.js` |

## Node mounting

Every node faces the centre of the triangle with **mic B on its right**. If a node is turned around, its lines come out mirrored.
