#!/usr/bin/env python3
# Serial capture for SuperFW UART debug builds.
#   gba-rawlog.py [LOGFILE [BAUD]]   (default: $GBA_LOG or ~/.cache/superfw-debug/raw.log)
# Port: /dev/ttyUSB0, or $GBA_PORT. Screenshots go to shots/ next to the log.
# - Logs everything received (hex + text) with timestamps.
# - Extracts "@@SCR1" screenshot blocks and renders them to shots/*.png.
# - Waits for the adapter and reconnects automatically if it is unplugged.
import serial, time, sys, os, struct

SCR_MAGIC = b"@@SCR1"
SCR_LEN = 2 + 512 + 240 * 160 + 512 + 1024 + 16384     # Payload after the magic
SCR_END = b"@@END\n"
SHOTS = None    # Set in main(), next to the log file
PORT = os.environ.get("GBA_PORT", "/dev/ttyUSB0")
LOCK = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"),
                    "superfw-xfer-%s.lock" % os.path.basename(PORT))


def locked():
    """True while a live gba-serial.py transfer owns the port (stale locks,
    left by killed processes, are removed)."""
    try:
        pid = int(open(LOCK).read().strip() or 0)
    except (OSError, ValueError):
        return os.path.exists(LOCK)
    if pid and not os.path.exists("/proc/%d" % pid):
        try:
            os.unlink(LOCK)
        except OSError:
            pass
        return False
    return True

OBJ_SIZES = {0: [(8, 8), (16, 16), (32, 32), (64, 64)],
             1: [(16, 8), (32, 8), (32, 16), (64, 32)],
             2: [(8, 16), (8, 32), (16, 32), (32, 64)]}

def rgb(v):
    return ((v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3)

def render(payload, path):
    from PIL import Image
    o = 0
    dispcnt = struct.unpack_from("<H", payload, o)[0]; o += 2
    bgpal = [rgb(v) for v in struct.unpack_from("<256H", payload, o)]; o += 512
    frame = payload[o:o + 240 * 160]; o += 240 * 160
    objpal = [rgb(v) for v in struct.unpack_from("<256H", payload, o)]; o += 512
    oam = payload[o:o + 1024]; o += 1024
    tiles = payload[o:o + 16384]
    img = Image.new("RGB", (240, 160))
    px = img.load()
    for y in range(160):
        for x in range(240):
            px[x, y] = bgpal[frame[y * 240 + x]]
    if dispcnt & 0x1000:                       # OBJ layer enabled
        for i in range(127, -1, -1):           # OBJ 0 drawn last (on top)
            a0, a1, a2 = struct.unpack_from("<HHH", oam, i * 8)
            if (a0 & 0x0300) == 0x0200 or (a0 >> 14) == 3:
                continue                       # Disabled / invalid shape
            mode = (a0 >> 10) & 3
            if mode == 2:
                continue                       # OBJ window, not drawn
            tile = a2 & 0x3FF
            if tile < 512:
                continue                       # Not usable in bitmap modes
            w, h = OBJ_SIZES[a0 >> 14][a1 >> 14]
            x0, y0 = a1 & 0x1FF, a0 & 0xFF
            if x0 >= 240: x0 -= 512
            if y0 >= 160: y0 -= 256
            c256, hf, vf = a0 & 0x2000, a1 & 0x1000, a1 & 0x2000
            for ty in range(h):
                for tx in range(w):
                    sx, sy = (w - 1 - tx if hf else tx), (h - 1 - ty if vf else ty)
                    X, Y = x0 + tx, y0 + ty
                    if not (0 <= X < 240 and 0 <= Y < 160):
                        continue
                    if c256:
                        tn = tile + ((sy // 8) * (w // 8) + sx // 8) * 2
                        off = tn * 32 - 0x4000 + (sy % 8) * 8 + (sx % 8)
                        if not 0 <= off < len(tiles): continue
                        ci = tiles[off]
                        col = objpal[ci] if ci else None
                    else:
                        tn = tile + (sy // 8) * (w // 8) + sx // 8
                        off = tn * 32 - 0x4000 + (sy % 8) * 4 + (sx % 8) // 2
                        if not 0 <= off < len(tiles): continue
                        nib = (tiles[off] >> 4) if sx & 1 else (tiles[off] & 15)
                        col = objpal[(a2 >> 12) * 16 + nib] if nib else None
                    if col is None:
                        continue
                    if mode == 1:              # Semi-transparent: ~50% blend
                        b = px[X, Y]
                        col = tuple((c + d) // 2 for c, d in zip(col, b))
                    px[X, Y] = col
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.resize((480, 320), Image.NEAREST).save(path)

def default_log():
    return os.environ.get("GBA_LOG", os.path.expanduser("~/.cache/superfw-debug/raw.log"))


def main():
    global SHOTS
    logpath = sys.argv[1] if len(sys.argv) > 1 else default_log()
    os.makedirs(os.path.dirname(os.path.abspath(logpath)), exist_ok=True)
    SHOTS = os.path.join(os.path.dirname(os.path.abspath(logpath)), "shots")
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
    with open(logpath, "a", buffering=1) as f:
        f.write("=== capture started %s @ %d baud ===\n" % (time.strftime("%H:%M:%S"), baud))
        waiting, buf = False, b""
        while True:
            if locked():                      # A file transfer owns the port
                time.sleep(0.2)
                continue
            try:
                with serial.Serial(PORT, baud, timeout=0.1) as s:
                    f.write("%s (adapter connected)\n" % time.strftime("%H:%M:%S"))
                    waiting = False
                    while not locked():
                        buf += s.read(65536)
                        # Screenshot blocks are cut out of the stream and rendered.
                        i = buf.find(SCR_MAGIC)
                        if i >= 0:
                            text, rest = buf[:i], buf[i + len(SCR_MAGIC):]
                            if len(rest) < SCR_LEN + len(SCR_END):
                                if text:
                                    f.write("%s [%d bytes] hex=%s\n%s\n" % (time.strftime("%H:%M:%S"), len(text), text[:64].hex(), text.decode("latin1")))
                                buf = buf[i:]
                                continue
                            name = os.path.join(SHOTS, "shot-%s.png" % time.strftime("%H%M%S"))
                            try:
                                render(rest[:SCR_LEN], name)
                                f.write("%s [screenshot saved: %s]\n" % (time.strftime("%H:%M:%S"), name))
                            except Exception as e:
                                f.write("%s [screenshot failed: %s]\n" % (time.strftime("%H:%M:%S"), e))
                            buf = text + rest[SCR_LEN + len(SCR_END):]
                        # Keep a possible partial magic at the end for the next read.
                        keep = 0
                        for k in range(len(SCR_MAGIC) - 1, 0, -1):
                            if buf.endswith(SCR_MAGIC[:k]):
                                keep = k; break
                        out, buf = (buf[:-keep], buf[-keep:]) if keep else (buf, b"")
                        if out:
                            f.write("%s [%d bytes] hex=%s\n%s\n" % (time.strftime("%H:%M:%S"), len(out), out[:64].hex(), out.decode("latin1")))
                f.write("%s (paused for a file transfer)\n" % time.strftime("%H:%M:%S"))
            except (serial.SerialException, OSError):
                if not waiting:
                    f.write("%s (adapter not available, waiting)\n" % time.strftime("%H:%M:%S"))
                    waiting = True
                time.sleep(2)

if __name__ == "__main__":
    main()
