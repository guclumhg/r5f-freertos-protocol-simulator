#!/usr/bin/env bash
# Cut power to the board's USB port and give it back.
#
# The sweep deliberately drives the receive interrupt past the point where it
# takes the whole core. When it gets there no task runs again - including the
# one that would have stopped it - and the board stops answering. It is not
# crashed and it is not in BOOTSEL, so neither picotool nor the 1200 baud
# reset can reach it. Cutting the port's power is the only way back, and it is
# enough: the firmware boots normally and only wedges once a sweep is started.
#
# Needs a hub with per-port power switching. Both hubs on the Jetson have it.
set -euo pipefail

command -v uhubctl >/dev/null || {
    echo "installing uhubctl"
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq uhubctl >/dev/null
}

# Find whichever port the Pico is on rather than hard-coding it.
LOC="$(sudo uhubctl 2>/dev/null | awk '
    /^Current status for hub/ { hub = $5 }
    /2e8a:/                   { gsub(":", "", $2); print hub, $2; exit }')"

[ -n "$LOC" ] || { echo "no Raspberry Pi device found on a switchable port"; exit 1; }

HUB="${LOC% *}"
PORT="${LOC#* }"
back_yet() {
    for _ in $(seq 1 "${1:-20}"); do
        sleep 0.5
        [ -e /dev/ttyACM0 ] && return 0
    done
    return 1
}

echo "power cycling $HUB port $PORT"
sudo uhubctl -l "$HUB" -p "$PORT" -a cycle --delay 3 >/dev/null 2>&1 || true
back_yet 24 && { echo "back on /dev/ttyACM0"; exit 0; }

# Cycling the one port sometimes leaves the port reporting "connect" with no
# device behind it - powered, but never enumerating. Cycling the whole hub
# clears that.
echo "port alone was not enough - cycling the whole hub"
sudo uhubctl -l "$HUB" -a cycle --delay 4 >/dev/null 2>&1 || true
back_yet 30 && { echo "back on /dev/ttyACM0"; exit 0; }

# Last resort: make the kernel re-enumerate the hub itself.
echo "still nothing - rebinding the hub driver"
echo -n "$HUB" | sudo tee /sys/bus/usb/drivers/usb/unbind >/dev/null 2>&1 || true
sleep 2
echo -n "$HUB" | sudo tee /sys/bus/usb/drivers/usb/bind >/dev/null 2>&1 || true
back_yet 30 && { echo "back on /dev/ttyACM0"; exit 0; }

echo "the board did not come back"
exit 1
