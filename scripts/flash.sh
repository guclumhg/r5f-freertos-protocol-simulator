#!/usr/bin/env bash
# Flash the firmware onto the Pico 2W.
#
# If the board is already in BOOTSEL it shows up as a small USB block device
# labelled RP2350. If it is running our firmware instead, opening its serial
# port at 1200 baud asks it to reboot into BOOTSEL.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UF2="$ROOT/firmware/build/r5f_sim.uf2"
[ -f "$UF2" ] || { echo "no firmware at $UF2 - run scripts/build.sh"; exit 1; }

find_bootsel() {
    lsblk -rno NAME,LABEL | awk '$2=="RP2350"||$2=="RPI-RP2"{print "/dev/"$1; exit}'
}

DEV="$(find_bootsel || true)"

if [ -z "$DEV" ]; then
    PORT="$(ls /dev/ttyACM* 2>/dev/null | head -1 || true)"
    if [ -n "$PORT" ]; then
        echo "asking $PORT to reboot into BOOTSEL"
        # 1200 baud with DTR dropped is the Pico's reset-to-bootloader signal
        stty -F "$PORT" 1200 hupcl || true
        for _ in $(seq 1 30); do
            sleep 0.5
            DEV="$(find_bootsel || true)"
            [ -n "$DEV" ] && break
        done
    fi
fi

[ -n "$DEV" ] || { echo "no BOOTSEL device found - hold BOOTSEL and replug"; exit 1; }

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
    PORT="$(ls /dev/ttyACM* 2>/dev/null | head -1 || true)"
    [ -n "$PORT" ] && { echo "up on $PORT"; exit 0; }
done
echo "flashed, but no serial port appeared yet"
