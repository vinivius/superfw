#!/usr/bin/env python3
# pad.py STEP...: drives an interactive RetroArch (player 1) through its
# Network RetroPad, so it works without window focus while the user watches.
# Needs in ~/.config/retroarch/retroarch.cfg: network_remote_enable = "true",
# network_remote_enable_user_p1 = "true" (UDP port 55400) and
# pause_nonactive = "false". Screenshots use hyprctl and grim (Hyprland).
#   down        tap a button (a b select start up down left right l r)
#   down*3      tap it 3 times
#   l+r+up      press a combo together
#   down:1.5    hold for 1.5 s
#   1.2         wait 1.2 s
#   shot:NAME   screenshot of the RetroArch window to $SHOTS/NAME.png
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
def shot(name):
    c = [w for w in json.loads(subprocess.check_output(["hyprctl", "clients", "-j"]))
         if w["class"] == "com.libretro.RetroArch"]
    if not c:
        sys.exit("pad.py: no RetroArch window")
    c = c[0]
    (x, y), (w, h) = c["at"], c["size"]
    out = os.path.join(os.environ.get("SHOTS", "."), name + ".png")
    subprocess.run(["grim", "-g", f"{x},{y} {w}x{h}", out], check=True)
    print("shot", out)
for step in sys.argv[1:]:
    if step.startswith("shot:"):
        shot(step[5:])
    elif step.replace(".", "").isdigit():
        time.sleep(float(step))
    elif ":" in step:
        n, t = step.split(":"); press([n], float(t))
    elif "*" in step:
        n, k = step.split("*")
        for _ in range(int(k)): press([n], 0.1)
    else:
        press(step.split("+"), 0.12 if "+" not in step else 0.4)
