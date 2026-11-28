# Node Board Setup

Notes from getting a Beagle running as a node on the project switch.
This picks up where `chrony-quick-setup.md` leaves off (that guide assumes the boards already have their 192.168.10.x addresses, but nothing sets them).

## Network

The BeagleBoard Debian Trixie image uses systemd-networkd (there's no NetworkManager or `nmcli`).
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
DHCP stays on as well, so the board still gets an address on a normal network.
To undo it, delete `acoustic.conf` and restart systemd-networkd.

Restarting systemd-networkd also drops the default route added by hand in step 1 of the chrony guide.
That's fine on the project switch (there's no internet there anyway), but it has to be added again before running `apt`.

## chrony

A node should only have `acoustic-node.conf` in `/etc/chrony/conf.d/`.
One of our nodes also had `acoustic-master.conf`, which tells it to serve its own clock (`local stratum 10 orphan`) when it can't reach the master.
That hides a broken link instead of showing it, so remove it:

```sh
sudo rm /etc/chrony/conf.d/acoustic-master.conf
sudo systemctl restart chrony
```

## Running the Node With a Replay

A replay plays a recording through the detector and estimator instead of the mics, so it tests the whole node without any hardware.
It also doesn't need `sudo` (it never touches the R5).
Build on the laptop and make a synthetic recording (it prints the angle each burst should give):

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=aarch64-toolchain.cmake
cmake --build build
python3 dsp/tools/make_replay.py replay.csv
```

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

## Troubleshooting

| What you see on the node | Likely cause |
|--------------------------|--------------|
| `^?` and `Reach 0` in `chronyc sources` | chrony has never heard back from the master. Check the network first. |
| `eth0` has only an `fe80::` address | No static address (see Network above). |
| `ping 192.168.10.1` says `Destination Host Unreachable` and `ip neigh` shows `192.168.10.1 INCOMPLETE` | The node's side is fine, but nothing on the switch answers for 192.168.10.1. The master is off, unplugged, or doesn't have that address on its Ethernet port (see `master/board-setup.md`). |
| `ping` works but `Reach` stays at 0 | The master's chrony isn't running, its `allow` line doesn't cover the node, or a firewall is blocking UDP port 123. |
| The ssh session over USB freezes when Ethernet is plugged in | The board dropped off the laptop's USB bus (it vanished from `lsusb`). The cause isn't confirmed yet (most likely a power brownout from running the board off a laptop port). `Enter`, `~`, `.` kills the frozen session. |

## Status Log

2026-11-28, node zta32 (192.168.10.11):

- `eth0` had no IPv4 address, so `chronyc sources` showed `^?` with `Reach 0`. Added the drop-in above and `eth0` now has 192.168.10.11/24.
- Removed the stray `acoustic-master.conf`.
- Still not synced at first. The node's ARP request for 192.168.10.1 got no answer (`INCOMPLETE`), so the problem was on the master's side of the switch.
- After the master was fixed, ping to 192.168.10.1 (jah28-beagle) worked with about 0.4 ms round trip.
- chrony then picked the master and stepped the node's clock by 836.5 s (it had been roughly 14 minutes behind).
- Synced: `^*` with `Reach 377`, `System clock synchronized: yes`, and the node within about 1.4 µs of the master (`chronyc tracking`).
- The master was at stratum 3 (so it was following internet time servers when this was tested), which puts the node at stratum 4.

The `+/- 38ms` that `chronyc sources` shows is chrony's worst case all the way back to the internet servers (most of it is the master's 70 ms root delay).
For the project only the offset between node and master matters, and that's the microsecond figure from `chronyc tracking`.

Later the same day, the node ran with a synthetic replay (`make_replay.py`, built from `ff04416`).
All 14 windows in the first 12 s came out at the expected angles with nothing refused, and the node connected to the master as node 1.
