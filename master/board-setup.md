# Master Board Setup

Notes from getting a Beagle running as the master on the project switch.
It's the master version of `node/board-setup.md`, and it also picks up where `chrony-quick-setup.md` leaves off.

## Network

The master has the same problem as the nodes.
The BeagleBoard Debian Trixie image asks for an address over DHCP on `eth0`, and the project switch has no DHCP server, so `eth0` never gets 192.168.10.1.
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

Restarting systemd-networkd drops any default route added by hand, so add it again before running `apt`.

## chrony

The master should only have `acoustic-master.conf` in `/etc/chrony/conf.d/` (step 3 of `chrony-quick-setup.md`).
If it also has `acoustic-node.conf`, delete it, since that file points the master at itself.
Leave the `pool` line on (the master follows the internet servers whenever it has internet).

```sh
ls /etc/chrony/conf.d/         # only acoustic-master.conf
sudo chronyc clients           # every node that has synced shows up here
```

## Running the Master

```sh
./master [port]                # port defaults to 35100
```

If the master runs a firewall, open TCP port 35100 (nodes) and UDP port 123 (chrony).

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
| A node's `ping 192.168.10.1` says `Destination Host Unreachable` | The master's `eth0` has no 192.168.10.1 (see Network above), or the master is off or unplugged. |
| `ip -br addr show eth0` says `NO-CARRIER` | The cable isn't plugged into the switch, or the switch is off. |
| A node can ping the master but its `Reach` stays at 0 | chrony isn't running on the master, the `allow` line doesn't cover 192.168.10.0/24, or a firewall is blocking UDP port 123. |
| `sudo chronyc clients` is empty or only shows localhost | No node has asked yet (check the node side first). |

## Status Log

2026-11-28:

- Nodes couldn't reach the master (`Destination Host Unreachable`).
- The master's `eth0` didn't have 192.168.10.1, since the switch has no DHCP server. After adding the drop-in above, the node could reach the master.
