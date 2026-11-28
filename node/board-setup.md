# Node Board Setup

Everything needed to get a Beagle running as a node on the project switch (the master's version is `master/board-setup.md`).
It was written on the BeagleBoard Debian Trixie image (step 7 covers boards on Debian 12).
It uses the IPs from the network plan (master 192.168.10.1, nodes 192.168.10.11 to .14).
`documentation/time-sync.md` in the design docs explains why each chrony setting is there.

## 1. Get the Board Online

chrony has to be installed while the board still has internet, since the project switch doesn't have any.
For a Beagle plugged into a laptop over USB, run the laptop half of `internetToTarget.sh` (course networking guide, section 2.2).
Then on the Beagle:

```sh
sudo ip route add default via 192.168.7.1
ping -c 2 1.1.1.1
```

If the ping says `Network is unreachable`, the route is missing (it's lost on every reboot and every restart of systemd-networkd).
If it times out instead, the laptop isn't forwarding.

If `apt` then fails with `Not live until ...` or `certificate verify failed`, the board's clock is in the past (it has no RTC battery).
If chrony is already installed, `sudo chronyc makestep` fixes it once `chronyc sources` shows a `^*`.
Otherwise set it by hand first, for example `sudo date -s "2026-11-28 12:00"`.

## 2. Install chrony

```sh
sudo apt update
sudo apt install chrony
```

This also removes `systemd-timesyncd`, so only one program is setting the clock.

## 3. Configure chrony

```sh
sudo sed -i 's/^pool/#pool/' /etc/chrony/chrony.conf
sudo tee /etc/chrony/conf.d/acoustic-node.conf <<'EOF'
server 192.168.10.1 iburst minpoll 2 maxpoll 4
makestep 1 -1
EOF
sudo systemctl restart chrony
```

The `sed` line comments out Debian's internet time servers, so the master is the only source.
`makestep 1 -1` lets the clock jump whenever it's more than 1 s off (a board with no RTC battery can boot with a very wrong time).

A node should only have `acoustic-node.conf` in `/etc/chrony/conf.d/`.
One of our nodes also had `acoustic-master.conf`, which tells it to serve its own clock (`local stratum 10 orphan`) when it can't reach the master.
That hides a broken link instead of showing it, so remove it if it's there:

```sh
sudo rm /etc/chrony/conf.d/acoustic-master.conf
sudo systemctl restart chrony
```

## 4. Give eth0 a Static Address

The Trixie image uses systemd-networkd (there's no NetworkManager or `nmcli`).
Its configs live in `/etc/systemd/network/`, one file per interface.
`eth0.network` asks for an address over DHCP, and the project switch has no DHCP server, so out of the box `eth0` only gets a link-local `fe80::` address.

With no 192.168.10.x address, traffic for the master follows the default route out `usb0` to the laptop and is dropped.
You can see this with:

```sh
ip -br addr show eth0          # only fe80:: means no address
ip route get 192.168.10.1      # "via 192.168.7.1 dev usb0" means it's going the wrong way
```

Give `eth0` a static address with a drop-in file, so the Beagle's own `eth0.network` stays untouched:

```sh
sudo mkdir -p /etc/systemd/network/eth0.network.d
sudo tee /etc/systemd/network/eth0.network.d/acoustic.conf <<'EOF'
[Network]
Address=192.168.10.11/24
EOF
sudo systemctl restart systemd-networkd
```

Use the node's own address from the network plan (.11 to .14).
Every node needs a different one, so check `ip -br addr show eth0` on each board before plugging it into the switch.
Two boards with the same address fight over it, and the master's replies end up at whichever board answered last.
DHCP stays on as well, so the board still gets an address on a normal network.
To undo it, delete `acoustic.conf` and restart systemd-networkd.

Restarting systemd-networkd also drops the default route from step 1.
That's fine on the project switch (there's no internet there anyway), but it has to be added again before running `apt`.

## 5. Check It

Give the node about a minute after it's plugged into the switch, then run:

```sh
chronyc sources -v    # 192.168.10.1 should have ^* next to it
chronyc tracking      # "System time" should be well under 1 ms
timedatectl           # "System clock synchronized: yes"
```

On the master, `sudo chronyc clients` should list the node.
In our testing `chronyc tracking` put the node within about 1.4 µs of the master (see the status log).

## 6. Running the Node

With microphones (`node/README.md` covers starting the sampler first):

```sh
sudo DSP_MIC_SPACING_M=<spacing in m> ./node <node ID> 192.168.10.1 [port]
```

### With a Replay

A replay plays a recording through the detector and estimator instead of the mics, so it tests the whole node without any hardware.
It also doesn't need `sudo` (it never touches the R5).
Build on the laptop and make a synthetic recording (it prints the angle each burst should give):

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=aarch64-toolchain.cmake
cmake --build build
python3 dsp/tools/make_replay.py replay.csv
```

`./build.sh` does the same build inside the podman container, and copies `node` to the NFS share.

Copy both to the node (or use the NFS share if it's mounted) and point the node at the master:

```sh
scp build/node/node replay.csv ztomas@192.168.7.2:acoustic/
cd ~/acoustic
DSP_REPLAY=replay.csv DSP_VERBOSE=1 ./node 1 192.168.10.1
```

The first argument is the node ID and the port defaults to 35100 (same as the master).
With `DSP_VERBOSE=1` every window is printed, and the angles should match what `make_replay.py` printed (0, ±14.9, ±31.0, and ±50.5 degrees).
The recording loops, so the same seven bursts repeat every 5.2 s.
Drop `DSP_VERBOSE` once it works, since the 10 s summary is enough.

## 7. Building for Older Boards

Not every board runs the same image.
Madden's board runs Debian 12 with the TI 6.1 kernel, so it only has glibc 2.36, and a node built on the laptop (Debian 13) won't start there.
A static build carries its own copy of the C library, so it runs on either image:

```sh
cmake -S . -B build-static -DCMAKE_TOOLCHAIN_FILE=aarch64-toolchain.cmake -DCMAKE_EXE_LINKER_FLAGS=-static
cmake --build build-static --target node
```

Check what a board has with `cat /etc/debian_version` and `ldd --version`.

## 8. Starting at Boot

`node/board/acoustic-node.service` starts the R5 and then the node at boot, and restarts the node if it crashes.
`install_node_service.sh` installs it (copy both to the board along with the node and the R5 files):

```sh
scp build-static/node/node node/board/acoustic-node.service node/board/install_node_service.sh \
    r5capture/board/r5_start.sh r5capture/board/mcspi_power_on.sh \
    r5capture/prebuilt/r5capture-mcspi.elf r5capture/prebuilt/r5cap_ctl \
    madden@192.168.7.2:acoustic/service/
```

Then on the board (stop any node you started by hand first, or the two will fight over the same ID):

```sh
cd ~/acoustic/service
sudo ./install_node_service.sh 2 0.065           # node ID, mic spacing in m, [master IP]
journalctl -u acoustic-node -f                   # follow its output
```

It copies everything to `/opt/acoustic-node` and puts the settings in `/etc/default/acoustic-node`.
Any `DSP_*` variable can go in that file too (run `sudo systemctl restart acoustic-node` after editing it).
To update the node binary, copy the new one over and run the install script again.

A few choices in the service file are worth knowing about:

- It doesn't wait for `network-online.target`. `eth0` keeps asking for DHCP on the project switch and never counts as online, so that would hold boot up for 2 minutes. The node retries the master every second anyway.
- It waits up to a minute for chrony to sync to the master (`chronyc waitsync`), so the first events have usable times. If chrony can't sync it starts the node anyway.
- If `r5_start.sh` fails (no overlay, for example) the service tries 5 times and then gives up. `systemctl status acoustic-node` shows why.
- A node that gets kicked off because another node has its ID exits cleanly, so systemd doesn't restart it.

## 9. Switching Back to Normal

For other courses or assignments, turn the internet servers back on and disable the node file (chrony only reads files in `conf.d/` that end in `.conf`):

```sh
sudo sed -i 's/^#pool/pool/' /etc/chrony/chrony.conf
sudo mv /etc/chrony/conf.d/acoustic-node.conf /etc/chrony/conf.d/acoustic-node.conf.off
sudo systemctl restart chrony
```

This needs internet (see step 1).
To go back to the project setup:

```sh
sudo sed -i 's/^pool/#pool/' /etc/chrony/chrony.conf
sudo mv /etc/chrony/conf.d/acoustic-node.conf.off /etc/chrony/conf.d/acoustic-node.conf
sudo systemctl restart chrony
```

The static address can stay, since DHCP still works alongside it.

## Troubleshooting

| What you see on the node | Likely cause |
|--------------------------|--------------|
| `^?` and `Reach 0` in `chronyc sources` | chrony has never heard back from the master. Check the network first. |
| An empty source list | chrony isn't reading the node file (it has to end in `.conf`) or wasn't restarted. |
| `sources with unknown address` in `chronyc activity` | The `pool` line is still active on a board with no internet. |
| `eth0` has only an `fe80::` address | No static address (see step 4). |
| `ping 192.168.10.1` says `Destination Host Unreachable` and `ip neigh` shows `192.168.10.1 INCOMPLETE` | The node's side is fine, but nothing on the switch answers for 192.168.10.1. The master is off, unplugged, or doesn't have that address on its Ethernet port (see `master/board-setup.md`). |
| `ping` works but `Reach` stays at 0 | The master's chrony isn't running, its `allow` line doesn't cover the node, or a firewall is blocking UDP port 123. |
| The ssh session over USB freezes when Ethernet is plugged in | The board dropped off the laptop's USB bus (it vanished from `lsusb`). The cause isn't confirmed yet (most likely a power brownout from running the board off a laptop port). `Enter`, `~`, `.` kills the frozen session. |
| `version 'GLIBC_2.38' not found (required by ./node)` | The board runs Debian 12 (glibc 2.36) and the laptop's cross-compiler links against Debian 13's newer glibc. Build a static node instead (see step 7). |

## Status Log

2026-11-28, node zta32 (192.168.10.11):

- `eth0` had no IPv4 address, so `chronyc sources` showed `^?` with `Reach 0`. Added the drop-in above and `eth0` now has 192.168.10.11/24.
- Removed the stray `acoustic-master.conf`.
- Still not synced at first. The node's ARP request for 192.168.10.1 got no answer (`INCOMPLETE`), so the problem was on the master's side of the switch.
- After the master was fixed (its `eth0` was missing 192.168.10.1, see `master/board-setup.md`), ping to 192.168.10.1 (jah28-beagle) worked with about 0.4 ms round trip.
- chrony then picked the master and stepped the node's clock by 836.5 s (it had been roughly 14 minutes behind).
- Synced: `^*` with `Reach 377`, `System clock synchronized: yes`, and the node within about 1.4 µs of the master (`chronyc tracking`).
- The master was at stratum 3 (so it was following internet time servers when this was tested), which puts the node at stratum 4.

The `+/- 38ms` that `chronyc sources` shows is chrony's worst case all the way back to the internet servers (most of it is the master's 70 ms root delay).
For the project only the offset between node and master matters, and that's the microsecond figure from `chronyc tracking`.

Later the same day, the node ran with a synthetic replay (`make_replay.py`, built from `ff04416`).
All 14 windows in the first 12 s came out at the expected angles with nothing refused, and the node connected to the master as node 1.

Node madden-byai, 2026-11-28:

- Debian 12.15, glibc 2.36, kernel 6.1.83-ti-arm64-r71. The R5 was already running and `r5_start.sh` started sampling at 20,000 frames/s.
- The laptop build failed with `GLIBC_2.38 not found`. A static build (above) starts fine.
- chrony was already synced to the master (`^*`, `Reach 377`, about 4 µs off).
- Its `eth0` drop-in had 192.168.10.11, the same address as zta32, so it needs to move to .12.
