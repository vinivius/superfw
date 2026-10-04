#!/bin/sh
# Downloads the Arm GNU toolchain (arm-none-eabi, no root needed) into
# $SUPERFW_DEV/toolchain (default ~/Work/superfw-dev/toolchain).
set -e
WORK=${SUPERFW_DEV:-$HOME/Work/superfw-dev}
URL=https://developer.arm.com/-/media/Files/downloads/gnu/14.3.rel1/binrel/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi.tar.xz
mkdir -p "$WORK"
[ -x "$WORK/toolchain/bin/arm-none-eabi-gcc" ] && { echo "already installed"; exit 0; }
curl -sL "$URL" | tar xJ -C "$WORK"
mv "$WORK/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi" "$WORK/toolchain"
"$WORK/toolchain/bin/arm-none-eabi-gcc" --version | head -1
