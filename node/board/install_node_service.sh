#!/bin/bash
# Install the node as a systemd service, so the R5 and the node start at boot.
#   sudo ./install_node_service.sh <node ID> <mic spacing m> [master IP]
# Run from a folder holding acoustic-node.service, node, and the R5 files
# (r5_start.sh, mcspi_power_on.sh, r5cap_ctl, r5capture-mcspi.elf).
# Running it again updates the files and settings (it rewrites /etc/default/acoustic-node).
# Undo:
#   sudo systemctl disable --now acoustic-node
#   sudo rm -r /opt/acoustic-node /etc/systemd/system/acoustic-node.service /etc/default/acoustic-node
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ $# -ge 2 ] || { echo "usage: sudo $0 <node ID> <mic spacing m> [master IP]"; exit 1; }
cd "$(dirname "$0")" || exit 1

DIR=/opt/acoustic-node
FILES="node r5_start.sh mcspi_power_on.sh r5cap_ctl r5capture-mcspi.elf"
for f in $FILES acoustic-node.service; do
  [ -e "$f" ] || { echo "$f missing here"; exit 1; }
done

systemctl stop acoustic-node 2>/dev/null   # can't overwrite a running binary
mkdir -p $DIR && cp $FILES $DIR/ && chmod +x $DIR/node $DIR/*.sh $DIR/r5cap_ctl || exit 1
cp acoustic-node.service /etc/systemd/system/ || exit 1

cat > /etc/default/acoustic-node <<EOF
# Settings for acoustic-node.service. Any DSP_* variable from node/README.md works here.
# After changing this: sudo systemctl restart acoustic-node
NODE_ID=$1
MASTER_IP=${3:-192.168.10.1}
DSP_MIC_SPACING_M=$2
EOF

systemctl daemon-reload
systemctl enable acoustic-node
# Match "node <ID> ..." only: the board's code-server also runs a process called node (Node.js)
if pgrep -f '(^|/)node [0-9]+ ' >/dev/null; then
  echo "Installed, but a node is already running by hand. Stop it, then: sudo systemctl start acoustic-node"
else
  systemctl start acoustic-node && echo "Installed and started. Follow it with: journalctl -u acoustic-node -f"
fi
