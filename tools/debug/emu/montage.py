#!/usr/bin/env python3
# montage.py OUT.png IN.png... : puts screenshots in a grid (3 per row, 240x160
# each, with their file names) to review many screens at once.
import sys, os
from PIL import Image, ImageDraw
ins = sys.argv[2:]
cols = 3
rows = (len(ins) + cols - 1) // cols
W, H, T = 240, 160, 12
out = Image.new("RGB", (cols * (W + 4), rows * (H + T + 4)), "black")
d = ImageDraw.Draw(out)
for i, p in enumerate(ins):
    x, y = (i % cols) * (W + 4), (i // cols) * (H + T + 4)
    out.paste(Image.open(p).convert("RGB").resize((W, H), Image.NEAREST), (x, y + T))
    d.text((x + 2, y), os.path.basename(p)[:38], fill="yellow")
out.save(sys.argv[1])
print(sys.argv[1])
