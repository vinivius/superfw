#!/bin/sh
# keys.sh DIR KEYS: injects keys into the emulator started by run.sh in DIR
# (any build, also in games): a b u d l r L R s e, [...] for combos. Each key
# takes 8 frames (held 4, released 4).
printf '%s' "$2" > "$1/fe.ctl"
