#!/usr/bin/env bash
# Build the firmware. Run on the machine the Pico is plugged into.
set -euo pipefail

# shellcheck source=/dev/null
[ -f "$HOME/.r5f-env" ] && source "$HOME/.r5f-env"

: "${PICO_SDK_PATH:?run scripts/setup.sh first}"
: "${FREERTOS_KERNEL_PATH:?run scripts/setup.sh first}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/firmware"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"

echo
ls -l build/r5f_sim.uf2 build/r5f_sim.elf
arm-none-eabi-size build/r5f_sim.elf
