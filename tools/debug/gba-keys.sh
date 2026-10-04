#!/bin/sh
# gba-keys.sh KEYS [WAIT]: send keys, wait WAIT seconds (default 3) and print the
# new firmware log lines, without the heartbeat (needs gba-rawlog.py running).
D=$(dirname "$0")
LOG=${GBA_LOG:-$HOME/.cache/superfw-debug/raw.log}
START=$(stat -c %s "$LOG")
python3 "$D/gba-serial.py" send "$1"
python3 -c "import time; time.sleep(${2:-3})"
tail -c +$((START + 1)) "$LOG" | grep -a "^\[src/" | grep -av "\] alive [0-9]*" | cut -c1-200
