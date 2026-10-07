#!/bin/sh
# flash-free.sh IMAGE [LABEL]: how much of the 512 KiB flash a firmware image
# leaves free. tools/fw-fixer.py pads superfw.gba with 0xFF up to the next 512
# byte block, so its size doesn't tell: this measures where the data ends.
python3 - "$1" "${2:-$1}" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
end = min(len(d.rstrip(b"\xff")), len(d.rstrip(b"\x00")))
print("- %s: %d of 524288 bytes used, **%d bytes free**" % (sys.argv[2], end, 524288 - end))
PY
