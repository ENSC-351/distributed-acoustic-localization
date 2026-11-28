# Running the master and web GUI

Two programs run on the master board (192.168.10.1 on the project switch):

- `master`: takes the nodes' direction reports (TCP port 35100), groups them per sound, triangulates, and answers the GUI on UDP port 12345
- `web/server.js`: Node.js web server on port 8088 that relays the browser to `master`

Board network and chrony setup: `master/board-setup.md` and `node/board-setup.md`.

## 0. Tell the master where the nodes are

Copy `master/layout.example.txt` to `layout.txt` and list every node: its ID (the one it was started with), where it sits in metres, and the way it faces.
Mount each node with mic B on its right when looking the way it faces, or its line comes out mirrored.
Without `--layout` the master assumes nodes 1-3 in a 2 m triangle, and reports from any other node ID are shown in the node table but not used.

## 1. Build (VM)

```bash
cd ~/ensc351/work/distributed-acoustic-localization
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=aarch64-toolchain.cmake
cmake --build build                       # copies master to ~/ensc351/public/myApps/
```

## 2. Copy the web server to the NFS share (VM)

```bash
cd ~/ensc351/work/distributed-acoustic-localization/web
npm install                               # first time only; makes node_modules/
mkdir -p ~/ensc351/public/myApps/dal-web
cp -r server.js package.json lib public node_modules ~/ensc351/public/myApps/dal-web/
cp ../master/layout.example.txt ~/ensc351/public/myApps/layout.txt   # then edit it
```

`node_modules` is plain JavaScript, so the copy from the VM works on the board (the board has no internet by default).

## 3. Run (board)

```bash
ssh madden@192.168.7.2
./mountNFS.sh                             # once per boot
/mnt/remote/myApps/master --layout /mnt/remote/myApps/layout.txt &   # live nodes
cd /mnt/remote/myApps/dal-web && node server.js
```

Open `http://192.168.7.2:8088` in a browser on the host.
Each node shows **up** in the Nodes table once it connects; start them as in `node/board-setup.md` (`./node <ID> 192.168.10.1`).
Add `--demo` to the master to also make up fake sounds (no nodes needed).

## Stopping

- In the page: **Terminate master** (stops `master`; the page then shows "no master")
- Or `Ctrl+C` / `kill %1` for `master`, and `Ctrl+C` for `node server.js`

## Run on the VM instead (no board)

Use a separate build folder so the ARM binary in `myApps` isn't replaced:

```bash
cd ~/ensc351/work/distributed-acoustic-localization
cmake -S . -B build-vm && cmake --build build-vm --target master node
./build-vm/master/master --demo &          # or without --demo, plus replay nodes:
#   DSP_REPLAY=replay.csv DSP_MIC_SPACING_M=0.065 ./build-vm/node/node 1 127.0.0.1
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
| Node IDs, positions, facings | `--layout <file>` (`master/layout.example.txt`); built-in: `LAYOUT_SIDE_M` in `master/include/layout.h` |
| Node port (35100) | `PROTO_PORT` in `common/include/common/protocol.h`, or `master <port>` |
| Recent sounds kept (10) | `DASHBOARD_FIX_HISTORY` in `master/include/dashboard.h` |
| Clock shown (UTC-7, 24 h) | `DISPLAY_UTC_OFFSET_H` in `web/public/javascripts/dal_ui.js` |
| Which reports count as one sound (150 ms) | `FUSION_WINDOW_MS` in `master/include/fusion.h` |
| Demo sound period (3 s) | `DEMO_PERIOD_MS` in `master/include/demo_source.h` |
| Quality limits (5 / 15 cm, placeholders) | `GOOD_SPREAD_M`, `FAIR_SPREAD_M` in `web/public/javascripts/dal_ui.js` |
| UDP port (12345) | `UDP_PORT` in `master/include/udp.h` and `MASTER_PORT` in `web/lib/dal_server.js` |
| Web port (8088) | `PORT_NUMBER` in `web/server.js` |

## Node mounting

Every node faces the centre of the triangle with **mic B on its right**. If a node is turned around, its lines come out mirrored.
