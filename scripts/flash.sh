#!/usr/bin/env bash
# Flash the firmware onto the Pico 2W, recovering the board first if it needs it.
#
# Four ways in, tried in order, because the sweep deliberately drives the board
# into a state where the first three do not work:
#
#   1. it is already in BOOTSEL
#   2. picotool asks it over the USB vendor interface
#   3. opening its serial port at 1200 baud asks it the old way
#   4. cut the power to its USB port, then try 2 and 3 again
#
# The fourth is what makes this unattended. A board saturated by its own
# receive interrupt runs no task, answers no request, and is not in BOOTSEL.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UF2="$ROOT/firmware/build/r5f_sim.uf2"
[ -f "$UF2" ] || { echo "no firmware at $UF2 - run scripts/build.sh"; exit 1; }

# The picotool the SDK builds is stripped down and has no reboot command.
PICOTOOL="$HOME/r5f-tools/picotool-full/build/picotool"
[ -x "$PICOTOOL" ] || PICOTOOL=""

find_bootsel() {
    lsblk -rno NAME,LABEL | awk '$2=="RP2350"||$2=="RPI-RP2"{print "/dev/"$1; exit}'
}

DEV=""
wait_bootsel() {
    for _ in $(seq 1 "${1:-20}"); do
        sleep 0.5
        DEV="$(find_bootsel || true)"
        [ -n "$DEV" ] && return 0
    done
    return 1
}

ask_nicely() {
    if [ -n "$PICOTOOL" ]; then
        echo "asking the board to reboot into BOOTSEL (picotool)"
        sudo "$PICOTOOL" reboot -f -u >/dev/null 2>&1 || true
        wait_bootsel 16 && return 0
    fi
    if [ -e /dev/ttyACM0 ]; then
        echo "asking /dev/ttyACM0 to reboot into BOOTSEL (1200 baud)"
        stty -F /dev/ttyACM0 1200 hupcl 2>/dev/null || true
        wait_bootsel 16 && return 0
    fi
    return 1
}

DEV="$(find_bootsel || true)"

if [ -z "$DEV" ]; then
    ask_nicely || true
fi

if [ -z "$DEV" ]; then
    echo "no response - cutting the port's power"
    bash "$ROOT/scripts/power-cycle.sh" || true
    sleep 2
    ask_nicely || true
fi

[ -n "$DEV" ] || { echo "still no BOOTSEL - hold the button and replug"; exit 1; }

MNT="$(mktemp -d)"
trap 'sudo umount "$MNT" 2>/dev/null || true; rmdir "$MNT" 2>/dev/null || true' EXIT

sudo mount "$DEV" "$MNT"
echo "flashing $(basename "$UF2") -> $DEV"
sudo cp "$UF2" "$MNT/"
sync
sudo umount "$MNT"

echo "waiting for the board to come back as a serial port"
for _ in $(seq 1 40); do
    sleep 0.5
    [ -e /dev/ttyACM0 ] && { echo "up on /dev/ttyACM0"; exit 0; }
done
echo "flashed, but no serial port appeared yet"
