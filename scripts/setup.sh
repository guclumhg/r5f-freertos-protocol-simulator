#!/usr/bin/env bash
# One command to go from a clean machine to a built .uf2.
#
# Everything that does not need root goes under ~/r5f-tools: the Arm
# toolchain, the Pico SDK and the FreeRTOS kernel. The only things that need
# sudo are three apt packages.
#
#     ./scripts/setup.sh
#
# Safe to re-run; it skips whatever is already in place.
set -euo pipefail

TOOLS="${R5F_TOOLS:-$HOME/r5f-tools}"
ARM_V=14.2.rel1
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

case "$(uname -m)" in
    x86_64)  HOST=x86_64 ;;
    aarch64) HOST=aarch64 ;;
    *) echo "unsupported host architecture: $(uname -m)"; exit 1 ;;
esac

ARM_DIR="$TOOLS/arm-gnu-toolchain-$ARM_V-$HOST-arm-none-eabi"
mkdir -p "$TOOLS"

echo "### 1/4  build dependencies"
if ! command -v ninja >/dev/null || ! pkg-config --exists libusb-1.0 2>/dev/null; then
    sudo apt-get update -qq
    # picotool, which the SDK uses to produce the .uf2, needs libusb.
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        ninja-build libusb-1.0-0-dev pkg-config cmake git >/dev/null
fi
echo "    cmake $(cmake --version | head -1 | awk '{print $3}'), ninja $(ninja --version)"

echo "### 2/4  arm-none-eabi toolchain ($HOST host)"
if [ ! -x "$ARM_DIR/bin/arm-none-eabi-gcc" ]; then
    URL="https://developer.arm.com/-/media/Files/downloads/gnu/$ARM_V/binrel/arm-gnu-toolchain-$ARM_V-$HOST-arm-none-eabi.tar.xz"
    echo "    downloading $ARM_V ..."
    curl -fL --retry 3 -o "$TOOLS/arm.tar.xz" "$URL"
    tar -xf "$TOOLS/arm.tar.xz" -C "$TOOLS"
    rm -f "$TOOLS/arm.tar.xz"
fi
echo "    $("$ARM_DIR/bin/arm-none-eabi-gcc" --version | head -1)"

echo "### 3/4  Pico SDK and FreeRTOS kernel"
if [ ! -d "$TOOLS/pico-sdk/.git" ]; then
    git clone -q --branch 2.3.0 --depth 1 \
        https://github.com/raspberrypi/pico-sdk.git "$TOOLS/pico-sdk"
fi
git -C "$TOOLS/pico-sdk" submodule update --init --depth 1 lib/tinyusb >/dev/null
# The RP2350 FreeRTOS port lives in Raspberry Pi's fork, not upstream:
# portable/ThirdParty/GCC/RP2350_ARM_NTZ
if [ ! -d "$TOOLS/FreeRTOS-Kernel/.git" ]; then
    git clone -q --depth 1 \
        https://github.com/raspberrypi/FreeRTOS-Kernel.git "$TOOLS/FreeRTOS-Kernel"
fi
[ -d "$TOOLS/FreeRTOS-Kernel/portable/ThirdParty/GCC/RP2350_ARM_NTZ" ] \
    || { echo "the FreeRTOS checkout has no RP2350 port"; exit 1; }
echo "    pico-sdk 2.3.0, FreeRTOS kernel with RP2350_ARM_NTZ"

cat > "$HOME/.r5f-env" <<EOF
export PICO_SDK_PATH="$TOOLS/pico-sdk"
export FREERTOS_KERNEL_PATH="$TOOLS/FreeRTOS-Kernel"
export PICO_TOOLCHAIN_PATH="$ARM_DIR"
export PATH="$ARM_DIR/bin:\$PATH"
EOF

echo "### 4/4  building the firmware"
# shellcheck source=/dev/null
source "$HOME/.r5f-env"
bash "$ROOT/scripts/build.sh"

cat <<EOF

setup complete.

    ./scripts/flash.sh              flash the board
    ./scripts/install-service.sh    dashboard as a service on :8000
    python3 server/app.py           or just run it in the foreground
EOF
