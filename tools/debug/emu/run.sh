#!/bin/sh
# run.sh DIR FIRMWARE [MINUTES]: runs FIRMWARE (ie. superfw.gba) in the headless
# emulator in real time, for MINUTES (default 30), with DIR/sdcard.img as the
# SD card. The UART pty path is written to DIR/uart.pty (use it as GBA_PORT),
# the last frame is saved to DIR/end.ppm. Needs setup.sh to have run.
WORK=${SUPERFW_DEV:-$HOME/Work/superfw-dev}
FW=$(realpath "$2")
[ -f "$1/sdcard.img" ] || { echo "no $1/sdcard.img"; exit 1; }
cd "$1" && rm -f uart.pty
FE_REALTIME=1 exec "$WORK/fe" "$WORK/gpsp-supercard/gpsp_libretro.so" "$FW" "$(( ${3:-30} * 3600 )):0:end.ppm"
