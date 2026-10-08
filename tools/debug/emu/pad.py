#!/usr/bin/env python3
# pad.py STEP...: drives an interactive RetroArch (player 1) through its
# Network RetroPad, so it works without window focus while the user watches.
# Needs in ~/.config/retroarch/retroarch.cfg: network_remote_enable = "true",
# network_remote_enable_user_p1 = "true" (UDP port 55400) and
# pause_nonactive = "false". Screenshots use hyprctl and grim (Hyprland).
#   down        tap a button (a b select start up down left right l r)
#   l+r+up      press a combo together
#   down*3      tap it (or a combo) 3 times
#   l+r:1.5     hold it (or a combo) for 1.5 s
#   1.2         wait 1.2 s
#   shot:NAME   screenshot of the RetroArch window to $SHOTS/NAME.png
# It stops with an error if RetroArch isn't running (ie. it crashed at
# startup), instead of sending presses nowhere.
import json, os, socket, struct, subprocess, sys, time
IDS = dict(b=0, y=1, select=2, start=3, up=4, down=5, left=6, right=7, a=8, x=9, l=10, r=11)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def send(name, state):
    # struct remote_message { int port, device, index, id; uint16_t state; } (20 bytes)
    s.sendto(struct.pack("<iiiiH2x", 0, 1, 0, IDS[name], state), ("127.0.0.1", 55400))

def press(names, hold):
    for n in names: send(n, 1)
    time.sleep(hold)
    for n in names: send(n, 0)
    time.sleep(0.22)

def window():
    c = [w for w in json.loads(subprocess.check_output(["hyprctl", "clients", "-j"]))
         if w["class"] == "com.libretro.RetroArch"]
    if not c:
        sys.exit("pad.py: no RetroArch window")
    return c[0]

def shot(name):
    c = window()
    (x, y), (w, h) = c["at"], c["size"]
    out = os.path.join(os.environ.get("SHOTS", "."), name + ".png")
    subprocess.run(["grim", "-g", f"{x},{y} {w}x{h}", out], check=True)
    print("shot", out)

def parse(step):
    """Button step -> (buttons, hold time, repeats)."""
    names, _, hold = step.partition(":")
    names, _, count = names.partition("*")
    keys = names.split("+")
    bad = [k for k in keys if k not in IDS]
    if bad or (count and not count.isdigit()):
        sys.exit(f"pad.py: bad step {step!r}")
    return keys, float(hold) if hold else (0.4 if len(keys) > 1 else 0.12), int(count or 1)

steps = sys.argv[1:]
presses = [parse(st) for st in steps
           if not st.startswith("shot:") and not st.replace(".", "").isdigit()]
if presses and subprocess.run(["pgrep", "-x", "retroarch"], capture_output=True).returncode:
    sys.exit("pad.py: RetroArch is not running")

for step in steps:
    if step.startswith("shot:"):
        shot(step[5:])
    elif step.replace(".", "").isdigit():
        time.sleep(float(step))
    else:
        keys, hold, count = parse(step)
        for _ in range(count):
            press(keys, hold)
