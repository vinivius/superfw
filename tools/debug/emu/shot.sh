#!/bin/sh
# shot.sh DIR [OUT.png]: saves the exact frame the emulator started by run.sh
# in DIR is showing (menu or game) as a PNG (default DIR/frame-HHMMSS.png)
# and prints its path.
OUT=${2:-$1/frame-$(date +%H%M%S).png}
rm -f "$1/fe-shot.ppm"
pkill -USR1 -f "^$(echo "${SUPERFW_DEV:-$HOME/Work/superfw-dev}" | sed 's/[.]/\\./g')/fe " || { echo "emulator not running"; exit 1; }
for i in $(seq 50); do [ -f "$1/fe-shot.ppm" ] && break; sleep 0.1 2>/dev/null || python3 -c "import time; time.sleep(0.1)"; done
python3 -c "from PIL import Image; import sys; Image.open(sys.argv[1]).resize((480, 320), Image.NEAREST).save(sys.argv[2])" "$1/fe-shot.ppm" "$OUT" && echo "$OUT"
