#!/bin/sh
# flash-free.sh IMAGE [LABEL]: how much of the 512 KiB flash a firmware image
# leaves free (superfw.gba is padded to 1 KiB, so its size doesn't tell).
python3 - "$1" "${2:-$1}" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
end = min(len(d.rstrip(b"\xff")), len(d.rstrip(b"\x00")))
print("- %s: %d of 524288 bytes used, **%d bytes free**" % (sys.argv[2], end, 524288 - end))
PY
