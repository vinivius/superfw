#!/bin/sh
# Builds the debugging emulator into $SUPERFW_DEV (default ~/Work/superfw-dev):
# gpsp from its Supercard emulation branch, with gpsp-supercard.patch applied
# (SD timing/size fixes, UART on a pty), and fe, the headless libretro frontend.
set -e
WORK=${SUPERFW_DEV:-$HOME/Work/superfw-dev}
D=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$WORK"
cd "$WORK"
if [ ! -d gpsp-supercard ]; then
  git clone -q -b supercard-emu-dirty https://github.com/davidgfnet/gpsp gpsp-supercard
  git -C gpsp-supercard checkout -q dbaf87b99547e538df62f330691693bd6d5f1c88
  git -C gpsp-supercard apply "$D/gpsp-supercard.patch"
fi
# The x86-64 dynarec bypasses the Supercard hooks: build the interpreter.
make -C gpsp-supercard -j8 HAVE_DYNAREC=0 >/dev/null 2>&1 || make -C gpsp-supercard HAVE_DYNAREC=0
gcc -O1 -I gpsp-supercard/libretro/libretro-common/include -o fe "$D/fe.c" -ldl
echo "core:     $WORK/gpsp-supercard/gpsp_libretro.so"
echo "frontend: $WORK/fe"
