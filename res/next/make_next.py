# "NEXT": original arcade-fighter style lettering (heavy, chiseled, italic),
# rising diagonally (vertical shear keeps strokes crisp), sunset colors.
#   make_next.py HEIGHT ITALIC RISE GAP OUT.png [CUTS]
# The shipped art: make_next.py 20 0.28 0.12 10 next-sf.png
# Then: res/next/next2c.py --boot src/res/next_boot src/res/next_art.h res/next/next-sf.png
from PIL import Image, ImageDraw
import numpy as np, sys

L = {
 "N": [(0,100),(0,8),(8,0),(26,0),(50,52),(50,0),(70,0),(70,92),(62,100),(46,100),(22,48),(22,100)],
 "E": [(8,0),(62,0),(58,20),(22,20),(22,40),(52,40),(48,58),(22,58),(22,80),(60,80),(56,100),(0,100),(0,8)],
 "X": [(0,0),(24,0),(36,30),(48,0),(72,0),(50,50),(72,100),(48,100),(36,70),(24,100),(0,100),(22,50)],
 "T": [(0,0),(70,0),(66,20),(46,20),(46,92),(38,100),(24,100),(24,20),(4,20)],
}
W_ = {"N": 70, "E": 62, "X": 72, "T": 70}

def render(word, h_px, italic, rise, gap, sc=10):
    total = sum(W_[c] for c in word) + gap * (len(word) - 1)
    Wu = total + 40 + 30
    Hu = 100 + rise * total + 40
    img = Image.new("L", (int(Wu * sc), int(Hu * sc)), 0)
    d = ImageDraw.Draw(img)
    ox, oy = 20, 20 + rise * total
    x = 0
    for c in word:
        pts = []
        for px, py in L[c]:
            gx = x + px
            X = ox + gx + italic * (100 - py)
            Y = oy + py - rise * gx
            pts.append((X * sc, Y * sc))
        d.polygon(pts, fill=255)
        x += W_[c] + gap
    bb = img.getbbox()
    crop = img.crop(bb)
    s = h_px / (100 * sc)
    size = (max(1, round(crop.width * s)), max(1, round(crop.height * s)))
    m = np.array(crop.resize(size, Image.BOX)) > 128
    # Letter-local height (0 top .. 1 bottom) for each output pixel
    H, W = m.shape
    t = np.zeros((H, W))
    for yy in range(H):
        for xx in range(W):
            Xu = (bb[0] / sc) + (xx + 0.5) / s / sc * sc / sc
            Xu = bb[0] / sc + (xx + 0.5) / (s * sc) * 1.0
            Yu = bb[1] / sc + (yy + 0.5) / (s * sc) * 1.0
            gx = Xu - ox
            py = Yu - oy + rise * gx
            t[yy, xx] = min(1, max(0, py / 100))
    return m, t

def dilate(m):
    out = m.copy()
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            out |= np.roll(np.roll(m, dy, 0), dx, 1)
    return out

def style(m, t, cuts):
    m = np.pad(m, 3); t = np.pad(t, 3)
    bands = [(255, 228, 92), (255, 182, 72), (255, 128, 88), (255, 80, 140), (196, 64, 190), (110, 54, 176)]
    dark = (43, 15, 69)
    H, W = m.shape
    out = np.zeros((H, W, 4), np.uint8)
    shadow = np.roll(np.roll(m, 1, 0), 1, 1) | np.roll(np.roll(m, 2, 0), 1, 1)
    out[(dilate(m) | shadow) & ~m] = (*dark, 255)
    for y in range(H):
        for x in range(W):
            if m[y, x]:
                tt = t[y, x]
                cut = any(abs(tt - c) < 0.035 for c in cuts)
                out[y, x] = (*(dark if cut else bands[min(5, int(tt * 6))]), 255)
    return Image.fromarray(out)

h, italic, rise, gap, out = int(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), sys.argv[5]
cuts = [float(c) for c in sys.argv[6].split(",")] if len(sys.argv) > 6 and sys.argv[6] else []
m, t = render("NEXT", h, italic, rise, gap)
im = style(m, t, cuts)
im = im.crop(im.getbbox())
im.save(out); print(out, im.size)
