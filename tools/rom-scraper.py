#!/usr/bin/env python3
#
# SuperFW SD card ROM scraper.
#
# Identifies GBA/GB/GBC ROMs on an SD card (CRC32 lookup against the No-Intro
# DATs shipped by libretro-database), renames them to their official No-Intro
# names (together with all the per-ROM files SuperFW keeps: saves, save backups,
# savestates, cheats, patches, per-ROM config, recent list entries...) and
# generates box-art thumbnails in the SuperFW .img format.
#
# Usage:
#   rom-scraper.py <sd_root> [--dry-run] [--no-rename] [--no-art] [--force-art] [--cache DIR]
#   rom-scraper.py --preview file.img out.png [--scale N]
#
# Requires Python 3.6+ and Pillow (only needed for art generation / preview).

import argparse
import collections
import io
import os
import re
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

# --- Constants ------------------------------------------------------------

DAT_URL = ("https://raw.githubusercontent.com/libretro/libretro-database/master/"
           "metadat/no-intro/{}.dat")
THUMB_URL = "https://thumbnails.libretro.com/{}/{}/{}.png"

SYS_GBA = "Nintendo - Game Boy Advance"
SYS_GB = "Nintendo - Game Boy"
SYS_GBC = "Nintendo - Game Boy Color"
SYSTEMS = [SYS_GBA, SYS_GB, SYS_GBC]

# Preferred system (DAT) for each ROM extension, used to break ties.
EXT_SYSTEMS = {
  ".gba": [SYS_GBA],
  ".gb":  [SYS_GB, SYS_GBC],
  ".gbc": [SYS_GBC, SYS_GB],
}
ROM_EXTS = tuple(EXT_SYSTEMS.keys())

DAT_MAX_AGE = 30 * 24 * 3600     # Refresh cached DATs after 30 days.
HTTP_TIMEOUT = 30
USER_AGENT = "superfw-rom-scraper/1.0"

# SuperFW paths (see src/config.h and src/settings.c)
SUPERFW_DIR = ".superfw"
ART_DIR = ".superfw/art"
SAVE_DIRS = ["SAVEGAME", "SAVES"]                       # settings.c save_paths[]
STATE_DIRS = ["SAVESTATE", ".superfw/savestate"]        # settings.c savestates_paths[]
CONFIG_DIR = ".superfw/config"                          # config.h ROMCONFIG_PATH
PATCHDB_DIR = ".superfw/patches"                        # config.h PATCHDB_PATH
RECENT_FILE = ".superfw/recent.txt"                     # config.h RECENT_FILEPATH
PENDING_SAVE_FILE = ".superfw/pending-save.txt"         # config.h PENDING_SAVE_FILEPATH
MAX_FN_LEN = 256                                        # config.h MAX_FN_LEN

# Characters invalid in FAT long file names.
FAT_INVALID = '\\/:*?"<>|'
# libretro-thumbnails naming: RetroArch gfx/gfx_thumbnail.c scrubs "&*/:`\"<>?\\|"
# (the libretro docs list &*/:`<>?\| ; the code also replaces the double quote).
THUMB_INVALID = '&*/:`<>?\\|"'

# Art format (see ART_FORMAT.md)
ART_MAGIC = b"SFWA"
ART_MAX_W = 80
ART_MAX_H = 80
ART_MAX_COLORS = 128


def log(msg=""):
  print(msg, flush=True)


def warn(msg):
  print("WARNING: " + msg, file=sys.stderr, flush=True)


# --- HTTP helpers ----------------------------------------------------------

class NotFound(Exception):
  pass


def http_get(url, retries=2):
  """Downloads a URL. Raises NotFound on 404, other exceptions on errors."""
  last = None
  for attempt in range(retries + 1):
    try:
      req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
      with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
        return resp.read()
    except urllib.error.HTTPError as e:
      if e.code == 404:
        raise NotFound(url)
      last = e
      if e.code < 500 and e.code != 429:
        break
    except (urllib.error.URLError, OSError) as e:
      last = e
    if attempt < retries:
      time.sleep(1 + attempt)
  raise last


def write_atomic(path, data):
  os.makedirs(os.path.dirname(path), exist_ok=True)
  tmp = path + ".part"
  with open(tmp, "wb") as fd:
    fd.write(data)
  os.replace(tmp, path)


# --- DAT parsing -----------------------------------------------------------

_TOKEN_RE = re.compile(r'"((?:[^"\\]|\\.)*)"|(\()|(\))|([^\s()"]+)')


def parse_clrmamepro(text):
  """Parses a clrmamepro DAT into a list of (blocktype, dict) entries.
     Nested blocks (ie. rom) are returned as lists of dicts under their key."""
  def tokens():
    for m in _TOKEN_RE.finditer(text):
      if m.group(1) is not None:
        yield ("str", m.group(1).replace('\\"', '"').replace('\\\\', '\\'))
      elif m.group(2):
        yield ("(", None)
      elif m.group(3):
        yield (")", None)
      else:
        yield ("word", m.group(4))

  it = tokens()

  def parse_block():
    d = {}
    key = None
    for kind, val in it:
      if kind == ")":
        return d
      if kind == "(":
        sub = parse_block()
        if key is not None:
          d.setdefault(key, []).append(sub)
          key = None
        continue
      if key is None:
        key = val
      else:
        if key in d and not isinstance(d[key], list):
          pass   # keep first value for duplicate scalar keys
        else:
          d.setdefault(key, val)
        key = None
    return d

  out = []
  pending = None
  for kind, val in it:
    if kind == "word" or kind == "str":
      pending = val
    elif kind == "(":
      blk = parse_block()
      out.append((pending, blk))
      pending = None
  return out


class GameDB(object):
  def __init__(self):
    self.by_crc = collections.defaultdict(list)    # crc -> [(system, name, size)]
    self.by_norm = collections.defaultdict(list)   # normalized name -> [(system, name)]
    self.by_short = collections.defaultdict(list)  # normalized short name -> [(system, name)]
    self.systems = []

  def add_dat(self, system, text):
    count = 0
    for btype, blk in parse_clrmamepro(text):
      if btype != "game":
        continue
      name = blk.get("name")
      if not name:
        continue
      for rom in blk.get("rom", []):
        crc = rom.get("crc")
        if crc and re.match(r"^[0-9A-Fa-f]{8}$", crc):
          size = int(rom["size"]) if rom.get("size", "").isdigit() else None
          self.by_crc[int(crc, 16)].append((system, name, size))
          count += 1
      self.by_norm[norm_name(name)].append((system, name))
      self.by_short[norm_name(short_name(name))].append((system, name))
    self.systems.append(system)
    return count

  def lookup_crc(self, crc, ext):
    cands = self.by_crc.get(crc, [])
    if not cands:
      return None
    prefs = EXT_SYSTEMS.get(ext, [])
    cands = sorted(cands, key=lambda c: prefs.index(c[0]) if c[0] in prefs else 99)
    return cands[0]


def norm_name(s):
  # Lowercase, treat FAT/thumbnail replacement chars as spaces, collapse spaces.
  s = s.lower()
  s = re.sub(r"[_\\/:*?\"<>|`]", " ", s)
  return re.sub(r"\s+", " ", s).strip()


def short_name(s):
  return s.split("(")[0].strip()


_BAD_TAGS = re.compile(r"\((?:[^)]*\b(?:beta|proto|demo|sample|kiosk|debug|pirate|unl|hack|"
                       r"virtual console|aftermarket|program|test)\b[^)]*)\)|\[b\]", re.I)
_REGION_PREF = ["usa", "world", "europe", "japan"]


def region_rank(name):
  tags = " ".join(re.findall(r"\(([^)]*)\)", name)).lower()
  for i, r in enumerate(_REGION_PREF):
    if r in tags:
      return i
  return len(_REGION_PREF)


def load_dats(cache_dir, offline=False):
  db = GameDB()
  datdir = os.path.join(cache_dir, "dats")
  os.makedirs(datdir, exist_ok=True)
  for system in SYSTEMS:
    path = os.path.join(datdir, system + ".dat")
    fresh = os.path.exists(path) and (time.time() - os.path.getmtime(path) < DAT_MAX_AGE)
    if not fresh and not offline:
      url = DAT_URL.format(urllib.parse.quote(system))
      try:
        data = http_get(url)
        if b"clrmamepro" not in data[:200] and b"game (" not in data:
          raise ValueError("downloaded file does not look like a clrmamepro DAT")
        write_atomic(path, data)
        log("Downloaded DAT: %s" % system)
      except Exception as e:
        warn("could not download DAT '%s': %s%s" % (
             system, e, " (using cached copy)" if os.path.exists(path) else ""))
    if not os.path.exists(path):
      warn("no DAT available for '%s', those ROMs will not be identified" % system)
      continue
    with open(path, "r", encoding="utf-8", errors="replace") as fd:
      n = db.add_dat(system, fd.read())
    log("Loaded %d entries from '%s' DAT" % (n, system))
  return db


# --- ROM scanning / identification ----------------------------------------

def crc32_file(path):
  crc = 0
  with open(path, "rb") as fd:
    while True:
      chunk = fd.read(1 << 20)
      if not chunk:
        break
      crc = zlib.crc32(chunk, crc)
  return crc & 0xFFFFFFFF


def scan_roms(sd_root):
  roms = []
  skip = os.path.normcase(os.path.join(os.path.abspath(sd_root), SUPERFW_DIR))
  for dirpath, dirnames, filenames in os.walk(sd_root):
    if os.path.normcase(os.path.abspath(dirpath)) == os.path.normcase(os.path.abspath(sd_root)):
      dirnames[:] = [d for d in dirnames if d.lower() != SUPERFW_DIR]
    dirnames.sort()
    for fn in sorted(filenames):
      if os.path.splitext(fn)[1].lower() in ROM_EXTS:
        full = os.path.join(dirpath, fn)
        if os.path.normcase(os.path.abspath(full)).startswith(skip + os.sep):
          continue
        if os.path.isfile(full):
          roms.append(full)
  return roms


class Rom(object):
  def __init__(self, path):
    self.path = path
    self.dir, self.fname = os.path.split(path)
    self.ext = os.path.splitext(self.fname)[1]
    self.crc = None
    self.system = None
    self.name = None          # No-Intro name
    self.match = None         # "crc", "filename", "fuzzy" or None
    self.final_fname = self.fname


def identify(rom, db):
  rom.crc = crc32_file(rom.path)
  hit = db.lookup_crc(rom.crc, rom.ext.lower())
  if hit:
    rom.system, rom.name, _ = hit
    rom.match = "crc"
    return
  # Filename fallback (exact normalized match against DAT names).
  stem = os.path.splitext(rom.fname)[0]
  prefs = EXT_SYSTEMS.get(rom.ext.lower(), [])
  cands = [c for c in db.by_norm.get(norm_name(stem), []) if c[0] in prefs]
  if cands:
    cands.sort(key=lambda c: prefs.index(c[0]))
    rom.system, rom.name = cands[0]
    rom.match = "filename"
    return
  # Fuzzy fallback: the file is named after the short title (no tags). Used
  # only to find box art, never to rename (the dump might be a hack/beta/...).
  cands = [c for c in db.by_short.get(norm_name(short_name(stem)), [])
           if c[0] in prefs and not _BAD_TAGS.search(c[1])]
  if cands and "(" not in stem:
    cands.sort(key=lambda c: (prefs.index(c[0]), region_rank(c[1]), len(c[1]), c[1]))
    rom.system, rom.name = cands[0]
    rom.match = "fuzzy"


# --- Renaming ----------------------------------------------------------------

def fat_sanitize(name):
  out = "".join("_" if (c in FAT_INVALID or ord(c) < 32) else c for c in name)
  out = out.rstrip(" .")
  return out or "_"


def sfw_stem(fname):
  """Mimics SuperFW util.c replace_extension(): strip the last extension."""
  i = fname.rfind(".")
  return fname[:i] if i >= 0 else fname


def dir_listing(path, cache):
  """Returns {lowercase name: real name} for a directory (cached)."""
  if path not in cache:
    try:
      cache[path] = {n.lower(): n for n in os.listdir(path)}
    except OSError:
      cache[path] = {}
  return cache[path]


def sd_path(sd_root, path):
  """Converts a host path to the SuperFW (FatFs) absolute path: /dir/file."""
  rel = os.path.relpath(path, sd_root).replace(os.sep, "/")
  return "/" + rel


class Renamer(object):
  def __init__(self, sd_root, dry_run):
    self.sd = sd_root
    self.dry = dry_run
    self.lcache = {}
    self.done = []       # (old, new) list of performed (or planned) renames
    self.skipped = []    # (path, reason)
    self.recent_map = {}         # old sd path (lower) -> new sd path
    self.pending_map = {}        # old save template (lower) -> new template

  def _exists(self, path):
    d, f = os.path.split(path)
    return f.lower() in dir_listing(d, self.lcache)

  def _do_rename(self, old, new):
    """Renames a file, never overwriting. Returns True on success."""
    od, of = os.path.split(old)
    nd, nf = os.path.split(new)
    case_only = (os.path.normcase(od) == os.path.normcase(nd) and of.lower() == nf.lower())
    if self._exists(new) and not case_only:
      same = False
      try:
        same = os.path.samefile(old, new)
      except OSError:
        pass
      if not same:
        self.skipped.append((old, "target exists: %s" % os.path.relpath(new, self.sd)))
        return False
    if not self.dry:
      try:
        if case_only:   # Case-only rename (FAT is case-insensitive): use a temp name
          tmp = old + ".sfwtmp"
          os.rename(old, tmp)
          os.rename(tmp, new)
        else:
          os.rename(old, new)
      except OSError as e:
        self.skipped.append((old, "rename failed: %s" % e))
        return False
    # Update the listing cache.
    dir_listing(od, self.lcache).pop(of.lower(), None)
    dir_listing(nd, self.lcache)[nf.lower()] = nf
    self.done.append((old, new))
    return True

  def _matching(self, dirpath, stem, suffix_re):
    """Files in dirpath named <stem><suffix> (case-insensitive)."""
    lst = dir_listing(dirpath, self.lcache)
    rx = re.compile(re.escape(stem.lower()) + "(" + suffix_re + ")$")
    out = []
    for lname, real in sorted(lst.items()):
      m = rx.match(lname)
      if m:
        out.append((real, real[len(stem):]))   # keep the original suffix spelling
    return out

  def related_files(self, rom, old_stem, shared_ok, local_ok):
    """Yields (dir, suffix_regex, kind) for files SuperFW associates with a ROM."""
    sav = r"\.sav|\.tmp\.sav|\.\d+\.sav"
    if local_ok:
      yield rom.dir, sav, "save"                 # save_path_policy=2 (next to ROM)
      yield rom.dir, r"\.cht", "cheats"          # menu.c prepare_gba_cheats()
      yield rom.dir, r"\.patch", "patch"         # patchengine.c load_rom_patches()
    if shared_ok:
      for d in SAVE_DIRS:
        yield os.path.join(self.sd, d), sav, "save"
      for d in STATE_DIRS:
        yield os.path.join(self.sd, d), r"\.\d+\.state", "savestate"
      yield os.path.join(self.sd, CONFIG_DIR), r"\.config", "config"
      yield os.path.join(self.sd, PATCHDB_DIR), r"\.patch", "patch-cache"

  def rename_rom(self, rom, new_fname, shared_ok, local_ok):
    old_path = rom.path
    new_path = os.path.join(rom.dir, new_fname)
    if not self._do_rename(old_path, new_path):
      return False
    rom.final_fname = new_fname
    old_stem, new_stem = sfw_stem(rom.fname), sfw_stem(new_fname)

    # Associated files (saves, backups, states, cheats, patches, config).
    for d, rx, kind in self.related_files(rom, old_stem, shared_ok, local_ok):
      for real, suffix in self._matching(d, old_stem, rx):
        self._do_rename(os.path.join(d, real), os.path.join(d, new_stem + suffix))
      if kind == "save":
        # pending-save.txt holds "<savedir>/<stem>" (no .sav), see save.c
        self.pending_map[sd_path(self.sd, os.path.join(d, old_stem)).lower()] = \
          sd_path(self.sd, os.path.join(d, new_stem))

    # Existing art is keyed by the full ROM file name.
    artdir = os.path.join(self.sd, ART_DIR)
    for real, suffix in self._matching(artdir, rom.fname, r"\.img"):
      self._do_rename(os.path.join(artdir, real), os.path.join(artdir, new_fname + suffix))

    self.recent_map[sd_path(self.sd, old_path).lower()] = sd_path(self.sd, new_path)
    return True

  def _rewrite_lines(self, relpath, mapper, what):
    path = os.path.join(self.sd, relpath)
    if not os.path.isfile(path):
      return
    try:
      with open(path, "rb") as fd:
        raw = fd.read()
    except OSError as e:
      warn("cannot read %s: %s" % (relpath, e))
      return
    text = raw.decode("utf-8", errors="surrogateescape")
    lines = text.split("\n")
    changed = 0
    for i, line in enumerate(lines):
      nl = mapper(line)
      if nl is not None and nl != line:
        lines[i] = nl
        changed += 1
    if changed:
      log("  %s: updating %d %s" % (relpath, changed, what))
      if not self.dry:
        try:
          write_atomic(path, "\n".join(lines).encode("utf-8", errors="surrogateescape"))
        except OSError as e:
          warn("cannot write %s: %s" % (relpath, e))

  def fix_superfw_lists(self):
    def recent_mapper(line):
      pfx = ""
      body = line.rstrip("\r")
      if body.startswith("nor:"):
        return None      # NOR games are not files on the SD card
      key = body if body.startswith("/") else "/" + body
      new = self.recent_map.get(key.lower())
      if new is None:
        return None
      if not body.startswith("/"):
        new = new[1:]
      return pfx + new + line[len(body):]

    def pending_mapper(line):
      body = line.rstrip("\r")
      new = self.pending_map.get(body.lower())
      return None if new is None else new + line[len(body):]

    if self.recent_map:
      self._rewrite_lines(RECENT_FILE, recent_mapper, "recent-games entries")
    if self.pending_map:
      self._rewrite_lines(PENDING_SAVE_FILE, pending_mapper, "pending save path(s)")


# --- Art encoding/decoding ----------------------------------------------------

def thumb_name(name):
  return "".join("_" if c in THUMB_INVALID else c for c in name)


def fetch_thumbnail(system, name, cache_dir, force):
  """Returns (png_bytes, kind) or (None, reason)."""
  tname = thumb_name(name)
  reasons = []
  for kind in ("Named_Boxarts", "Named_Titles"):
    cdir = os.path.join(cache_dir, "thumbs", system, kind)
    cpath = os.path.join(cdir, fat_sanitize(tname) + ".png")
    miss = cpath + ".404"
    if os.path.exists(cpath) and os.path.getsize(cpath) > 0:
      with open(cpath, "rb") as fd:
        return fd.read(), kind
    if os.path.exists(miss) and not force and time.time() - os.path.getmtime(miss) < DAT_MAX_AGE:
      reasons.append("%s: not found (cached)" % kind)
      continue
    url = THUMB_URL.format(*(urllib.parse.quote(x) for x in (system, kind, tname)))
    try:
      data = http_get(url)
      if not data.startswith(b"\x89PNG"):
        raise ValueError("not a PNG")
      write_atomic(cpath, data)
      if os.path.exists(miss):
        os.unlink(miss)
      return data, kind
    except NotFound:
      reasons.append("%s: not found" % kind)
      try:
        os.makedirs(cdir, exist_ok=True)
        open(miss, "wb").close()
      except OSError:
        pass
    except Exception as e:
      reasons.append("%s: %s" % (kind, e))
  return None, "; ".join(reasons)


def _c5(v):
  return (v * 31 + 127) // 255


def _c8(v5):
  return (v5 << 3) | (v5 >> 2)


def _pixels(im):
  f = getattr(im, "get_flattened_data", None)    # Pillow >= 12.1
  return f() if f else im.getdata()


def art_is_valid(path):
  try:
    with open(path, "rb") as fd:
      validate_art(fd.read())
    return True
  except (OSError, ValueError):
    return False


def encode_art(png_bytes):
  """Converts an image to the SuperFW .img format. Returns (bytes, w, h, ncolors)."""
  from PIL import Image

  img = Image.open(io.BytesIO(png_bytes))
  img.load()
  # Flatten any transparency on black (the format has no transparency).
  img = img.convert("RGBA")
  bg = Image.new("RGBA", img.size, (0, 0, 0, 255))
  img = Image.alpha_composite(bg, img).convert("RGB")

  w, h = img.size
  scale = min(ART_MAX_W / w, ART_MAX_H / h)
  nw = int(round(w * scale / 2.0)) * 2
  nw = max(2, min(ART_MAX_W, nw))
  nh = max(1, min(ART_MAX_H, int(round(h * scale))))
  img = img.resize((nw, nh), Image.LANCZOS)

  # 1. Compute an optimized palette (no dithering at this stage).
  method = Image.Quantize.MEDIANCUT
  try:
    from PIL import features
    if features.check("libimagequant"):
      method = Image.Quantize.LIBIMAGEQUANT
  except Exception:
    pass
  try:
    q = img.quantize(colors=ART_MAX_COLORS, method=method, kmeans=4 if method == Image.Quantize.MEDIANCUT else 0)
  except Exception:
    q = img.quantize(colors=ART_MAX_COLORS, method=Image.Quantize.MEDIANCUT, kmeans=4)
  pal = q.getpalette()[:3 * ART_MAX_COLORS]
  used = sorted(set(_pixels(q)))

  # 2. Snap palette to BGR555 (the hardware precision) and dedupe.
  colors = []
  for i in used:
    r, g, b = pal[3 * i:3 * i + 3]
    c = (_c5(r), _c5(g), _c5(b))
    if c not in colors:
      colors.append(c)
  colors = colors[:ART_MAX_COLORS]

  # 3. Remap the image to the snapped palette with Floyd-Steinberg dithering.
  palimg = Image.new("P", (1, 1))
  flat = []
  for c in colors:
    flat.extend(_c8(x) for x in c)
  flat.extend(flat[:3] * (256 - len(colors)))     # pad with duplicates of entry 0
  palimg.putpalette(flat)
  out = img.quantize(palette=palimg, dither=Image.Dither.FLOYDSTEINBERG)
  idx = list(_pixels(out))

  # Canonicalize indices (duplicates may have been selected) and drop unused colors.
  full = [tuple(flat[3 * i:3 * i + 3]) for i in range(256)]
  c8 = [tuple(_c8(x) for x in c) for c in colors]
  canon = {col: i for i, col in reversed(list(enumerate(c8)))}
  idx = [canon[full[i]] for i in idx]
  used = sorted(set(idx))
  remap = {o: n for n, o in enumerate(used)}
  idx = bytes(remap[i] for i in idx)
  colors = [colors[i] for i in used]

  hdr = ART_MAGIC + struct.pack("<HHHH", nw, nh, len(colors), 0)
  palb = b"".join(struct.pack("<H", r | (g << 5) | (b << 10)) for r, g, b in colors)
  data = hdr + palb + idx
  validate_art(data)
  return data, nw, nh, len(colors)


def validate_art(data):
  """Strict check against ART_FORMAT.md. Returns (w, h, palette, pixels)."""
  if len(data) < 12 or data[:4] != ART_MAGIC:
    raise ValueError("bad magic")
  w, h, n, res = struct.unpack_from("<HHHH", data, 4)
  if not (2 <= w <= ART_MAX_W and w % 2 == 0):
    raise ValueError("bad width %d" % w)
  if not (1 <= h <= ART_MAX_H):
    raise ValueError("bad height %d" % h)
  if not (1 <= n <= ART_MAX_COLORS):
    raise ValueError("bad color count %d" % n)
  if res != 0:
    raise ValueError("reserved field is not zero")
  exp = 12 + 2 * n + w * h
  if len(data) != exp:
    raise ValueError("bad file size %d (expected %d)" % (len(data), exp))
  pal = struct.unpack_from("<%dH" % n, data, 12)
  if any(c & 0x8000 for c in pal):
    raise ValueError("palette entry with bit 15 set")
  pix = data[12 + 2 * n:]
  if max(pix) >= n:
    raise ValueError("pixel index out of range")
  return w, h, pal, pix


def preview(img_path, out_png, scale=1):
  from PIL import Image
  with open(img_path, "rb") as fd:
    data = fd.read()
  w, h, pal, pix = validate_art(data)
  rgb = [(_c8(c & 31), _c8((c >> 5) & 31), _c8((c >> 10) & 31)) for c in pal]
  im = Image.new("RGB", (w, h))
  im.putdata([rgb[i] for i in pix])
  if scale > 1:
    im = im.resize((w * scale, h * scale), Image.NEAREST)
  im.save(out_png)
  log("%s: OK, %dx%d, %d colors, %d bytes -> %s" % (img_path, w, h, len(pal), len(data), out_png))


# --- Main --------------------------------------------------------------------

def main():
  if "--preview" in sys.argv[1:]:
    ap = argparse.ArgumentParser(prog="rom-scraper.py --preview",
                                 description="Decode/verify a SuperFW .img art file into a PNG")
    ap.add_argument("--preview", nargs=2, metavar=("IMG", "PNG"), required=True)
    ap.add_argument("--scale", type=int, default=1, help="integer upscale factor for the PNG")
    a = ap.parse_args()
    try:
      preview(a.preview[0], a.preview[1], max(1, a.scale))
    except (OSError, ValueError) as e:
      print("ERROR: %s" % e, file=sys.stderr)
      return 1
    return 0

  ap = argparse.ArgumentParser(
    description="Identify, rename and fetch box art for GBA/GB/GBC ROMs on a SuperFW SD card.",
    epilog="Preview mode: rom-scraper.py --preview file.img out.png [--scale N]")
  ap.add_argument("sd_root", help="SD card root directory")
  ap.add_argument("--dry-run", action="store_true", help="show what would be done, change nothing")
  ap.add_argument("--no-rename", action="store_true", help="do not rename ROMs (or their files)")
  ap.add_argument("--no-art", action="store_true", help="do not generate box art")
  ap.add_argument("--force-art", action="store_true", help="regenerate existing art files")
  ap.add_argument("--cache", default=os.path.expanduser("~/.cache/superfw-scraper"),
                  help="cache dir for DATs and thumbnails (default: %(default)s)")
  args = ap.parse_args()

  sd = os.path.abspath(args.sd_root)
  if not os.path.isdir(sd):
    print("ERROR: '%s' is not a directory" % args.sd_root, file=sys.stderr)
    return 1
  cache = os.path.abspath(os.path.expanduser(args.cache))

  if not args.no_art and not args.dry_run:
    try:
      import PIL  # noqa: F401
    except ImportError:
      print("ERROR: Pillow is required for art generation (pip install Pillow), "
            "or use --no-art", file=sys.stderr)
      return 1

  if args.dry_run:
    log("*** DRY RUN: nothing will be changed ***")

  db = load_dats(cache)
  paths = scan_roms(sd)
  log("Found %d ROM file(s) under %s\n" % (len(paths), sd))
  if not paths:
    return 0

  # Identify
  roms = []
  for p in paths:
    rom = Rom(p)
    try:
      identify(rom, db)
    except OSError as e:
      warn("cannot read %s: %s" % (p, e))
      continue
    roms.append(rom)
    rel = os.path.relpath(p, sd)
    if rom.match:
      log("[%-8s] %s (crc %08X) -> %s" % (rom.match, rel, rom.crc, rom.name))
    else:
      log("[no match] %s (crc %08X)" % (rel, rom.crc))

  # Rename
  ren = Renamer(sd, args.dry_run)
  if not args.no_rename:
    # SuperFW keys global save/state/config files by ROM file stem only, so
    # files are shared between ROMs with the same stem anywhere on the card.
    stems_global = collections.Counter(sfw_stem(r.fname).lower() for r in roms)
    stems_local = collections.Counter((os.path.normcase(r.dir), sfw_stem(r.fname).lower()) for r in roms)
    taken_stems = set(stems_global)
    log("\n== Renaming ==")
    for rom in roms:
      if rom.match not in ("crc", "filename"):
        continue
      new_fname = fat_sanitize(rom.name) + rom.ext
      if new_fname == rom.fname:
        continue
      rel = os.path.relpath(rom.path, sd)
      newsd = sd_path(sd, os.path.join(rom.dir, new_fname))
      if len(newsd.encode("utf-8")) >= MAX_FN_LEN - 16:
        ren.skipped.append((rom.path, "new path too long for SuperFW (%d bytes)" % len(newsd.encode())))
        continue
      old_stem = sfw_stem(rom.fname).lower()
      new_stem = sfw_stem(new_fname).lower()
      shared_ok = stems_global[old_stem] == 1
      local_ok = stems_local[(os.path.normcase(rom.dir), old_stem)] == 1
      if not shared_ok:
        warn("%s: other ROMs share the name '%s'; files in SAVEGAME/SAVES/SAVESTATE/.superfw "
             "are left untouched" % (rel, sfw_stem(rom.fname)))
      if not local_ok:
        warn("%s: another ROM in this folder shares its save name; local .sav/.cht/.patch "
             "left untouched" % rel)
      n0 = len(ren.done)
      if ren.rename_rom(rom, new_fname, shared_ok, local_ok):
        if new_stem in taken_stems and new_stem != old_stem:
          warn("%s: another ROM already uses the name '%s'; SuperFW will share save files "
               "between them" % (rel, sfw_stem(new_fname)))
        taken_stems.add(new_stem)
        for old, new in ren.done[n0:]:
          log("  %s\n    -> %s" % (os.path.relpath(old, sd), os.path.relpath(new, sd)))
    ren.fix_superfw_lists()
    if not ren.done:
      log("  (nothing to rename)")

  # Art
  art_written, art_skipped, art_failed = 0, 0, []
  if not args.no_art:
    log("\n== Box art ==")
    artdir = os.path.join(sd, ART_DIR)
    for rom in roms:
      if not rom.name:
        continue
      out = os.path.join(artdir, rom.final_fname + ".img")
      if not args.force_art and os.path.exists(out):
        if art_is_valid(out):
          art_skipped += 1
          continue
        log("  %s: existing file is not a valid art file, regenerating" % os.path.relpath(out, sd))
      if args.dry_run:
        log("  would create %s" % os.path.relpath(out, sd))
        continue
      png, kind = fetch_thumbnail(rom.system, rom.name, cache, args.force_art)
      if png is None:
        art_failed.append((rom.final_fname, kind))
        log("  %s: no art (%s)" % (rom.final_fname, kind))
        continue
      try:
        data, w, h, n = encode_art(png)
        write_atomic(out, data)
        art_written += 1
        log("  %s: %s %dx%d, %d colors" % (os.path.relpath(out, sd), kind, w, h, n))
      except Exception as e:
        art_failed.append((rom.final_fname, str(e)))
        log("  %s: conversion failed (%s)" % (rom.final_fname, e))

  # Summary
  log("\n== Summary ==")
  ident = [r for r in roms if r.match in ("crc", "filename")]
  log("ROMs scanned: %d, identified: %d (crc: %d, filename: %d), fuzzy (art only): %d" % (
      len(roms), len(ident), sum(r.match == "crc" for r in roms),
      sum(r.match == "filename" for r in roms), sum(r.match == "fuzzy" for r in roms)))
  if not args.no_rename:
    log("Files %srenamed: %d" % ("to be " if args.dry_run else "", len(ren.done)))
    for old, new in ren.done:
      log("  %s -> %s" % (os.path.relpath(old, sd), os.path.basename(new)))
    for p, why in ren.skipped:
      log("  SKIPPED %s: %s" % (os.path.relpath(p, sd), why))
  if not args.no_art and not args.dry_run:
    log("Art written: %d, already present: %d, failed: %d" % (art_written, art_skipped, len(art_failed)))
  unmatched = [r for r in roms if not r.match]
  if unmatched:
    log("Unmatched ROMs (%d):" % len(unmatched))
    for r in unmatched:
      log("  %s (crc %08X)" % (os.path.relpath(r.path, sd), r.crc))
  return 0


if __name__ == "__main__":
  sys.exit(main())
