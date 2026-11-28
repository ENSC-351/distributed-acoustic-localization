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
- Still not synced. The node's ARP request for 192.168.10.1 gets no answer (`INCOMPLETE`), so the problem is now on the master's side of the switch.
- The node's clock was roughly 14 minutes behind the laptop. `makestep 1 -1` should jump it once chrony syncs.
