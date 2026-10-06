#!/usr/bin/env python3
# Builds a demo SD card tree for screenshots (README, docs): 30 made-up games
# with original covers drawn here, so no real game names or box art show up.
#
#   demo-card.py OUTDIR [superfw.gba]
#
# OUTDIR gets GBA/*.gba (dummy ROMs: a valid header taken from the SuperFW
# image, a made-up title and game code, a save type marker, zeros), the box art
# in .superfw/art/XX/ and covers/*.png. To use it in the emulator, put it on a
# FAT32 image next to an empty GB/ GBC/ NES/ SAVEGAME/, ie.:
#
#   truncate -s 512M sdcard.img && mkfs.fat -F 32 -n SUPERFW sdcard.img
#   udisksctl loop-setup -f sdcard.img, copy GBA/ and .superfw/, unmount.
#
# Game codes start with 'Z' (unused by retail games), so the patch database
# never matches them. Needs Pillow.
import io, math, os, random, sys
from PIL import Image, ImageDraw, ImageFont
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))
import superfw_romlib as lib

OUT = sys.argv[1]
FW = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "..", "..", "..", "superfw.gba")
FB = "/usr/share/fonts/noto/NotoSans-Black.ttf"
if not os.path.exists(FB):
    FB = "/usr/share/fonts/noto/NotoSans-Bold.ttf"

GAMES = [  # title, motif, colors (top, bottom, accent), save, size MiB
  ("Astro Courier", "planet", ((20, 24, 70), (120, 60, 160), (255, 200, 80)), "FLASH1M_V103", 4),
  ("Bubble Bandits", "bubbles", ((40, 170, 220), (20, 60, 140), (255, 255, 255)), "SRAM_V113", 4),
  ("Castle of Clocks", "castle", ((250, 170, 90), (120, 40, 90), (60, 30, 60)), "EEPROM_V124", 8),
  ("Comet Kitchen", "planet", ((10, 10, 40), (40, 90, 150), (255, 120, 120)), "SRAM_V113", 4),
  ("Dune Drifter", "dunes", ((255, 200, 120), (230, 110, 60), (150, 60, 40)), "SRAM_V113", 8),
  ("Echo Garden", "forest", ((150, 230, 200), (40, 120, 90), (20, 70, 50)), "FLASH_V126", 4),
  ("Frost Fortress", "mountains", ((180, 220, 255), (90, 140, 210), (240, 250, 255)), "SRAM_V113", 8),
  ("Galaxy Gardener", "planet", ((30, 10, 60), (180, 80, 200), (120, 255, 160)), "FLASH1M_V103", 16),
  ("Harbor Heroes", "sea", ((255, 180, 120), (60, 100, 180), (30, 40, 90)), "SRAM_V113", 4),
  ("Island Hopper", "sea", ((120, 210, 255), (20, 140, 200), (250, 220, 120)), "SRAM_V113", 4),
  ("Jelly Jump", "bubbles", ((255, 150, 200), (160, 60, 160), (255, 240, 120)), "EEPROM_V124", 2),
  ("Knight Lantern", "castle", ((30, 30, 80), (90, 60, 140), (255, 210, 90)), "SRAM_V113", 8),
  ("Lunar Lanes", "grid", ((20, 10, 50), (200, 60, 160), (255, 220, 120)), "SRAM_V113", 8),
  ("Moss and Moonlight", "forest", ((20, 40, 80), (40, 110, 110), (240, 240, 200)), "FLASH1M_V103", 16),
  ("Neon Nomads", "grid", ((10, 10, 40), (60, 20, 120), (0, 255, 220)), "SRAM_V113", 8),
  ("Ocean Orchestra", "sea", ((90, 200, 230), (20, 60, 140), (255, 255, 255)), "SRAM_V113", 4),
  ("Pixel Parade", "city", ((255, 210, 120), (240, 100, 120), (80, 40, 100)), "SRAM_V113", 4),
  ("Pocket Planets", "planet", ((15, 20, 60), (60, 140, 200), (255, 170, 60)), "FLASH_V126", 8),
  ("Pocket Potions", "bubbles", ((120, 60, 160), (40, 20, 80), (140, 255, 140)), "SRAM_V113", 4),
  ("Pocket Puzzlebox", "grid", ((255, 230, 140), (240, 140, 60), (120, 60, 200)), "EEPROM_V124", 2),
  ("Quiet Quasar", "planet", ((5, 5, 20), (30, 40, 90), (200, 220, 255)), "SRAM_V113", 8),
  ("Rocket Rodeo", "dunes", ((255, 160, 80), (200, 60, 40), (90, 40, 30)), "FLASH1M_V103", 16),
  ("Sky Lanterns", "city", ((40, 30, 90), (230, 120, 90), (255, 220, 120)), "SRAM_V113", 4),
  ("Tin Can Tanks", "dunes", ((180, 210, 140), (110, 140, 70), (60, 70, 40)), "SRAM_V113", 4),
  ("Umbrella Unicorns", "mountains", ((255, 200, 230), (170, 140, 255), (255, 255, 255)), "SRAM_V113", 8),
  ("Velvet Valley", "mountains", ((255, 190, 150), (140, 70, 120), (70, 40, 90)), "FLASH_V126", 8),
  ("Wind Whistlers", "sea", ((200, 240, 255), (120, 180, 230), (255, 255, 255)), "SRAM_V113", 4),
  ("Xylo Express", "city", ((120, 220, 255), (60, 90, 200), (255, 230, 90)), "SRAM_V113", 8),
  ("Yarn Yeti", "mountains", ((210, 240, 255), (130, 170, 230), (255, 140, 160)), "EEPROM_V124", 4),
  ("Zephyr Zone", "grid", ((30, 60, 120), (20, 200, 200), (255, 255, 255)), "SRAM_V113", 8),
]

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

def shade(c, f):
    return tuple(max(0, min(255, int(v * f))) for v in c)

def cover(title, motif, cols, seed):
    W, H = 240, 264
    rnd = random.Random(seed)
    top, bot, acc = cols
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    for y in range(H):
        d.line([(0, y), (W, y)], fill=lerp(top, bot, y / H))
    dark = shade(bot, 0.45)
    if motif in ("planet", "grid", "city") or top[0] + top[1] + top[2] < 200:
        for _ in range(40):
            x, y = rnd.randrange(W), rnd.randrange(int(H * 0.7))
            d.point((x, y), fill=(255, 255, 230))
    if motif == "planet":
        cx, cy, r = W * 0.62, H * 0.58, 62
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=acc)
        d.ellipse((cx - r + 14, cy - r + 10, cx + r - 30, cy + r - 40), fill=shade(acc, 1.15))
        d.arc((cx - r - 40, cy - 22, cx + r + 40, cy + 22), 200, 340, fill=(255, 255, 255), width=5)
        d.ellipse((30, 170, 58, 198), fill=shade(top, 2.2))
    elif motif == "mountains":
        d.ellipse((150, 90, 200, 140), fill=acc)
        for i, (x, h, f) in enumerate(((-40, 120, 0.7), (60, 150, 0.85), (140, 110, 0.6))):
            c = shade(bot, f)
            d.polygon([(x, H), (x + 90, H - h - 40), (x + 180, H)], fill=c)
            d.polygon([(x + 70, H - h - 17), (x + 90, H - h - 40), (x + 110, H - h - 17)], fill=(250, 250, 255))
    elif motif == "sea":
        d.ellipse((90, 120, 170, 200), fill=acc)
        for i in range(6):
            y = 175 + i * 16
            pts = [(x, y + 6 * math.sin(x / 18 + i)) for x in range(0, W + 8, 8)]
            d.polygon(pts + [(W, H), (0, H)], fill=shade(bot, 0.95 - i * 0.08))
    elif motif == "bubbles":
        for _ in range(16):
            r = rnd.randrange(8, 34)
            x, y = rnd.randrange(W), rnd.randrange(70, H)
            d.ellipse((x - r, y - r, x + r, y + r), outline=acc, width=3)
            d.ellipse((x - r // 2, y - r // 2, x - r // 4, y - r // 4), fill=acc)
    elif motif == "castle":
        base = H - 70
        d.rectangle((40, base, 200, H), fill=acc)
        for x in (40, 100, 160):
            d.rectangle((x, base - 60, x + 40, H), fill=acc)
            d.polygon([(x - 6, base - 60), (x + 20, base - 100), (x + 46, base - 60)], fill=shade(acc, 1.6))
            d.rectangle((x + 15, base - 40, x + 25, base - 25), fill=(255, 220, 120))
        d.ellipse((96, base + 20, 144, base + 68), fill=(255, 240, 200))
        d.line((120, base + 44, 120, base + 30), fill=acc, width=3)
        d.line((120, base + 44, 132, base + 44), fill=acc, width=3)
    elif motif == "dunes":
        d.ellipse((140, 80, 200, 140), fill=(255, 245, 210))
        for i, f in enumerate((0.9, 0.75, 0.6)):
            y0 = 160 + i * 30
            pts = [(x, y0 + 18 * math.sin(x / 40 + i * 2)) for x in range(0, W + 8, 8)]
            d.polygon(pts + [(W, H), (0, H)], fill=shade(acc if i == 2 else bot, f + 0.2))
    elif motif == "forest":
        d.ellipse((150, 60, 196, 106), fill=acc)
        for row, f in ((150, 0.7), (180, 0.55), (210, 0.4)):
            for x in range(-10, W + 20, 34):
                xx = x + rnd.randrange(-8, 8)
                d.polygon([(xx, H), (xx + 20, row - rnd.randrange(0, 30)), (xx + 40, H)], fill=shade(bot, f))
    elif motif == "grid":
        hz = 170
        d.ellipse((70, hz - 70, 170, hz + 30), fill=acc)
        for i in range(5):
            d.rectangle((70, hz - 40 + i * 12, 170, hz - 36 + i * 12), fill=lerp(top, bot, (hz - 40 + i * 12) / H))
        d.rectangle((0, hz, W, H), fill=shade(top, 0.8))
        for i in range(-12, 13):
            d.line((W / 2 + i * 12, hz, W / 2 + i * 60, H), fill=acc, width=2)
        y, step = hz, 6
        while y < H:
            d.line((0, y, W, y), fill=acc, width=2)
            step *= 1.35
            y += step
    elif motif == "city":
        d.ellipse((150, 50, 200, 100), fill=(255, 245, 220))
        x = 0
        while x < W:
            w, h = rnd.randrange(22, 44), rnd.randrange(60, 150)
            d.rectangle((x, H - h, x + w, H), fill=acc)
            for wy in range(H - h + 8, H - 8, 14):
                for wx in range(x + 5, x + w - 6, 10):
                    if rnd.random() < 0.6:
                        d.rectangle((wx, wy, wx + 4, wy + 6), fill=(255, 230, 140))
            x += w + 2
    # Title: up to two lines, white with a dark outline, at the top.
    words = title.split()
    lines = [title] if len(title) <= 11 else [" ".join(words[:(len(words) + 1) // 2]), " ".join(words[(len(words) + 1) // 2:])]
    size = 46
    font = ImageFont.truetype(FB, size)
    while max(d.textlength(l, font=font) for l in lines) > W - 20 and size > 20:
        size -= 2
        font = ImageFont.truetype(FB, size)
    y = 14
    for l in lines:
        tw = d.textlength(l, font=font)
        d.text(((W - tw) / 2, y), l, font=font, fill=(255, 255, 255), stroke_width=5, stroke_fill=dark)
        y += size + 4
    return im

def rom_bytes(fwhdr, title, code, save, mib):
    hdr = bytearray(fwhdr[:0xC0])
    t = title.upper().replace(" ", "")[:12].encode("ascii").ljust(12, b"\0")
    hdr[0xA0:0xAC] = t
    hdr[0xAC:0xB0] = code.encode("ascii")
    hdr[0xB0:0xB2] = b"00"
    hdr[0xBC] = 0                      # Version
    chk = 0x19
    for i in range(0xA0, 0xBD):
        chk += hdr[i]
    hdr[0xBD] = (-chk) & 0xFF
    body = bytearray(mib * 1024 * 1024 - 0xC0)
    s = (save + "\0\0\0\0").encode("ascii")
    body[0x1000:0x1000 + len(s)] = s
    return bytes(hdr) + bytes(body)

fwhdr = open(FW, "rb").read(0xC0)
os.makedirs(os.path.join(OUT, "GBA"), exist_ok=True)
os.makedirs(os.path.join(OUT, "covers"), exist_ok=True)
for n, (title, motif, cols, save, mib) in enumerate(GAMES):
    fname = "%s.gba" % title
    code = "Z%c%cE" % (chr(65 + n // 26), chr(65 + n % 26))
    open(os.path.join(OUT, "GBA", fname), "wb").write(rom_bytes(fwhdr, title, code, save, mib))
    im = cover(title, motif, cols, n)
    im.save(os.path.join(OUT, "covers", title + ".png"))
    buf = io.BytesIO(); im.save(buf, "PNG")
    art = lib.encode_art(buf.getvalue())[0]
    p = os.path.join(OUT, lib.art_relpath(fname))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "wb").write(art)
print(len(GAMES), "games")
