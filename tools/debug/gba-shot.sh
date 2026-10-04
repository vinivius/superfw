#!/bin/sh
# gba-shot.sh [KEYS [WAIT]]: optionally send keys (and wait WAIT seconds), then
# take a menu screenshot and print the path of the PNG (needs gba-rawlog.py running).
D=$(dirname "$0")
LOG=${GBA_LOG:-$HOME/.cache/superfw-debug/raw.log}
if [ -n "$1" ]; then python3 "$D/gba-serial.py" send "$1"; python3 -c "import time; time.sleep(${2:-1.5})"; fi
N=$(grep -ac "screenshot saved" "$LOG" 2>/dev/null)   # grep -c prints 0 (and fails) on no match
N=${N:-0}
python3 "$D/gba-serial.py" send P
for i in $(seq 40); do
  python3 -c "import time; time.sleep(0.5)"
  C=$(grep -ac 'screenshot saved' "$LOG" 2>/dev/null)
  [ "${C:-0}" -gt "$N" ] && break
done
grep -a "screenshot saved" "$LOG" | tail -1 | sed 's/.*saved: \(.*\)\]/\1/'
