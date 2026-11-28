# Master Board Setup

Everything needed to get a Beagle running as the master on the project switch (the nodes' version is `node/board-setup.md`).
It assumes the BeagleBoard Debian Trixie image and the IPs from the network plan (master 192.168.10.1, nodes 192.168.10.11 to .14).
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
sudo tee /etc/chrony/conf.d/acoustic-master.conf <<'EOF'
local stratum 10 orphan
allow 192.168.10.0/24
EOF
sudo systemctl restart chrony
```

The first line lets the master serve its own clock when it has no internet.
The second lets the nodes ask it for the time.
Leave the `pool` line in `chrony.conf` on, so the master follows the internet servers whenever it's online.

The master should only have `acoustic-master.conf` in `/etc/chrony/conf.d/`.
If it also has `acoustic-node.conf`, delete it, since that file points the master at itself.

## 4. Give eth0 a Static Address

The master has the same problem as the nodes.
The Trixie image asks for an address over DHCP on `eth0`, and the project switch has no DHCP server, so `eth0` never gets 192.168.10.1.
From a node this looks like `Destination Host Unreachable` (with `ip neigh` showing `192.168.10.1 INCOMPLETE`), since nothing on the switch answers for that address.

Give `eth0` the master's address with a drop-in file (the same one the nodes use, with `.1`):

```sh
sudo mkdir -p /etc/systemd/network/eth0.network.d
sudo tee /etc/systemd/network/eth0.network.d/acoustic.conf <<'EOF'
[Network]
Address=192.168.10.1/24
EOF
sudo systemctl restart systemd-networkd
```

Check it with `ip -br addr show eth0`, which should list `192.168.10.1/24`.
DHCP stays on, so the board still gets an address on a normal network.
To undo it, delete `acoustic.conf` and restart systemd-networkd.

Restarting systemd-networkd drops the default route from step 1, so add it again before running `apt`.

## 5. Check It

```sh
ls /etc/chrony/conf.d/         # only acoustic-master.conf
sudo chronyc clients           # every node that has synced shows up here
```

`chronyc clients` needs root, since chrony only shares its client list with root.

## 6. Running the Master

```sh
./master [port]                # port defaults to 35100
```

If the master runs a firewall, open TCP port 35100 (nodes) and UDP port 123 (chrony).

The master's config doesn't need to change for other courses or assignments.
It still follows the internet servers whenever it's online, and the static address works alongside DHCP.

## Logging In Without a Password

`ssh` can't ask for a password without a terminal, so scripts (and the `!` commands in Claude Code) fail with `Permission denied`.
Copy a key to each board once from the laptop:

```sh
ls ~/.ssh/id_*.pub || ssh-keygen -t ed25519
ssh-copy-id jack@192.168.7.2
```

## Troubleshooting

| What you see | Likely cause |
|--------------|--------------|
| A node's `ping 192.168.10.1` says `Destination Host Unreachable` | The master's `eth0` has no 192.168.10.1 (see step 4), or the master is off or unplugged. |
| `ip -br addr show eth0` says `NO-CARRIER` | The cable isn't plugged into the switch, or the switch is off. |
| A node can ping the master but its `Reach` stays at 0 | chrony isn't running on the master, the `allow` line doesn't cover 192.168.10.0/24, or a firewall is blocking UDP port 123. |
| `sudo chronyc clients` is empty or only shows localhost | No node has asked yet (check the node side first). |

## Status Log

2026-11-28:

- Nodes couldn't reach the master (`Destination Host Unreachable`).
- The master's `eth0` didn't have 192.168.10.1, since the switch has no DHCP server. After adding the drop-in above, the node could reach the master.
