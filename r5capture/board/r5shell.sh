#!/bin/bash
# Send commands to the Zephyr shell on the R5 (over RPMsg) and print the replies.
# Usage on the BeagleY-AI:  sudo ./r5shell.sh "kernel uptime" "kernel thread list" "r5cap"
TTY=${TTY:-$(ls /dev/ttyRPMSG* 2>/dev/null | head -1)}
[ -n "$TTY" ] || { echo "no /dev/ttyRPMSG*: is the R5 loaded (with NO_STOP=1)?"; exit 1; }
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
stty -F "$TTY" raw -echo
for cmd in "$@"; do
  echo "### $cmd"
  # Bounded, and not holding our stdout: a blocked tty write must never hang the caller
  ( sleep 0.3; timeout 2 sh -c 'printf "%s\r\n" "$1" > "$2"' _ "$cmd" "$TTY" ) > /dev/null 2>&1 &
  timeout 1.5 cat "$TTY" | tr -d '\r'
  echo
done
