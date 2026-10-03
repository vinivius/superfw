#!/usr/bin/env python3
#
# SuperFW ROM library: shared engine for rom-scraper.py and rom-manager-gui.py.
#
# - No-Intro DAT download/parsing (libretro-database) and CRC32 identification
#   (NES/PCE: CRC computed with and without the copier header).
# - FAT-safe naming helpers.
# - libretro-thumbnails download (cached in ~/.cache/superfw-scraper) and
#   conversion to the SuperFW box-art .img format (docs/boxart-format.md).
# - Organizer: builds a fresh SuperFW SD card layout from a ROM collection
#   (one folder per console, official names, saves, cheats, box art), using
#   thread pools for hashing, copying and art generation.
#
# Requires Python 3.7+; Pillow is needed for art generation / preview.

import collections
import io
import json
import os
import re
import struct
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zlib
from concurrent.futures import ThreadPoolExecutor, as_completed

# --- Constants ------------------------------------------------------------

DAT_URL = ("https://raw.githubusercontent.com/libretro/libretro-database/master/"
           "metadat/no-intro/{}.dat")
THUMB_URL = "https://thumbnails.libretro.com/{}/{}/{}.png"

SYS_GBA = "Nintendo - Game Boy Advance"
SYS_GB = "Nintendo - Game Boy"
SYS_GBC = "Nintendo - Game Boy Color"
SYS_NES = "Nintendo - Nintendo Entertainment System"
SYS_SMS = "Sega - Master System - Mark III"
SYS_GG = "Sega - Game Gear"
SYS_SG = "Sega - SG-1000"
SYS_SV = "Watara - Supervision"
SYS_NGPC = "SNK - Neo Geo Pocket Color"
SYS_PCE = "NEC - PC Engine - TurboGrafx 16"

# Consoles SuperFW can run (natively or via the emulators listed in src/emu.c).
# folder: output folder; ext: canonical extension; exts: input extensions;
# systems: DATs to search (in preference order).
Console = collections.namedtuple("Console", "folder ext exts systems")
CONSOLES = [
  Console("GBA",  ".gba", (".gba", ".agb"), [SYS_GBA]),
  Console("GB",   ".gb",  (".gb", ".sgb", ".dmg"), [SYS_GB, SYS_GBC]),
  Console("GBC",  ".gbc", (".gbc", ".cgb"), [SYS_GBC, SYS_GB]),
  Console("NES",  ".nes", (".nes",), [SYS_NES]),
  Console("SMS",  ".sms", (".sms",), [SYS_SMS, SYS_GG, SYS_SG]),
  Console("GG",   ".gg",  (".gg",), [SYS_GG, SYS_SMS]),
  Console("SG",   ".sg",  (".sg",), [SYS_SG, SYS_SMS]),
  Console("SV",   ".sv",  (".sv",), [SYS_SV]),
  Console("NGPC", ".ngc", (".ngc",), [SYS_NGPC]),
  Console("PCE",  ".pce", (".pce",), [SYS_PCE]),
]
CONSOLE_BY_FOLDER = {c.folder: c for c in CONSOLES}
EXT_CONSOLE = {e: c.folder for c in CONSOLES for e in c.exts}
SYSTEM_CONSOLE = {SYS_GBA: "GBA", SYS_GB: "GB", SYS_GBC: "GBC", SYS_NES: "NES",
                  SYS_SMS: "SMS", SYS_GG: "GG", SYS_SG: "SG", SYS_SV: "SV",
                  SYS_NGPC: "NGPC", SYS_PCE: "PCE"}
SYSTEMS = list(SYSTEM_CONSOLE.keys())

# Preferred systems (DATs) for each ROM extension, used to break ties.
EXT_SYSTEMS = {e: c.systems for c in CONSOLES for e in c.exts}
ROM_EXTS = tuple(EXT_SYSTEMS.keys())

DAT_MAX_AGE = 30 * 24 * 3600     # Refresh cached DATs after 30 days.
HTTP_TIMEOUT = 20
USER_AGENT = "superfw-rom-scraper/2.0"
DEFAULT_CACHE = os.path.expanduser("~/.cache/superfw-scraper")
THUMB_MAX_CONCURRENCY = 8        # Be polite with thumbnails.libretro.com

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
# Longest file name (UTF-8 bytes) we generate. Leaves room for SuperFW's
# derived paths (eg. "/.superfw/savestate/<stem>.9.state", "/.superfw/art/<fn>.img").
MAX_NAME_BYTES = 200

# Characters invalid in FAT long file names.
FAT_INVALID = '\\/:*?"<>|'
# libretro-thumbnails naming: RetroArch gfx/gfx_thumbnail.c scrubs "&*/:`\"<>?\\|"
THUMB_INVALID = '&*/:`<>?\\|"'

# Art format (see docs/boxart-format.md)
ART_MAGIC = b"SFWA"
ART_MAX_W = 80
ART_MAX_H = 80
ART_MAX_COLORS = 128

PART_SUFFIX = ".sfwpart"
_PART_RE = re.compile(r"\.~[0-9a-f]{6}\.sfwpart$")

_GBA_LOGO = bytes.fromhex("24FFAE51699AA2213D84820A84E409AD")
_GB_LOGO = bytes.fromhex("CEED6666CC0D000B03730083000C000D")


def log(msg=""):
  print(msg, flush=True)


def warn(msg):
  print("WARNING: " + msg, file=sys.stderr, flush=True)


# --- HTTP / file helpers --------------------------------------------------------

class NotFound(Exception):
  pass


class Cancelled(Exception):
  pass


def http_get(url, retries=2, cancel=None):
  """Downloads a URL. Raises NotFound on 404, other exceptions on errors."""
  last = None
  for attempt in range(retries + 1):
    if cancel is not None and cancel.is_set():
      raise Cancelled()
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


def tmp_name(path):
  """Unique temporary name next to 'path' (same filesystem -> atomic rename)."""
  return "%s.~%s%s" % (path, uuid.uuid4().hex[:6], PART_SUFFIX)


def write_atomic(path, data):
  d = os.path.dirname(path)
  if d:
    os.makedirs(d, exist_ok=True)
  tmp = tmp_name(path)
  try:
    with open(tmp, "wb") as fd:
      fd.write(data)
    os.replace(tmp, path)
  except BaseException:
    try:
      os.unlink(tmp)
    except OSError:
      pass
    raise


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


def norm_name(s):
  # Lowercase, treat FAT/thumbnail replacement chars as spaces, collapse spaces.
  s = s.lower()
  s = re.sub(r"[_\\/:*?\"<>|`]", " ", s)
  return re.sub(r"\s+", " ", s).strip()


def short_name(s):
  return re.split(r"[(\[]", s)[0].strip()


def loose_name(s):
  """Very loose title key: short title, no articles/punctuation."""
  s = short_name(s).lower().replace("&", " and ").replace("'", "")
  words = re.findall(r"\w+", s)
  return "".join(w for w in words if w not in ("the", "a", "an"))


_BAD_TAGS = re.compile(r"\((?:[^)]*\b(?:beta|proto|demo|sample|kiosk|debug|pirate|unl|hack|"
                       r"virtual console|aftermarket|program|test)\b[^)]*)\)|\[b\]", re.I)
_REGION_PREF = ["usa", "world", "europe", "japan"]
_GOODTOOLS_REGIONS = {"u": "usa", "ue": "usa", "us": "usa", "ju": "usa", "uj": "usa",
                      "e": "europe", "eu": "europe", "j": "japan", "w": "world",
                      "f": "france", "g": "germany", "s": "spain", "i": "italy",
                      "k": "korea", "c": "china", "ch": "china"}
_HACK_RE = re.compile(r"hack|\[h\d*[^\]]*\]|\(h\)|\[t\d*[^\]]*\]", re.I)


def region_rank(name):
  tags = " ".join(re.findall(r"\(([^)]*)\)", name)).lower()
  for i, r in enumerate(_REGION_PREF):
    if r in tags:
      return i
  return len(_REGION_PREF)


def region_hint(stem):
  """Region named by a file's tags, GoodTools or No-Intro style (or None)."""
  for tag in re.findall(r"\(([^)]*)\)", stem):
    t = tag.strip().lower()
    if t in _GOODTOOLS_REGIONS:
      return _GOODTOOLS_REGIONS[t]
    for r in ("usa", "europe", "japan", "world"):
      if r in t:
        return r
  return None


class GameDB(object):
  def __init__(self):
    self.by_crc = collections.defaultdict(list)    # crc -> [(system, name, size)]
    self.by_norm = collections.defaultdict(list)   # normalized name -> [(system, name)]
    self.by_short = collections.defaultdict(list)  # normalized short name -> [(system, name)]
    self.by_loose = collections.defaultdict(list)  # loose title -> [(system, name)]
    self.by_serial = collections.defaultdict(list) # (system, serial) -> [(system, name)]
    self.systems = []

  def add_dat(self, system, text):
    count = 0
    seen = set()
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
      if name in seen:     # NES DAT lists each game twice (.nes and .unh)
        continue
      seen.add(name)
      self.by_norm[norm_name(name)].append((system, name))
      self.by_short[norm_name(short_name(name))].append((system, name))
      self.by_loose[loose_name(name)].append((system, name))
      serial = blk.get("serial")
      if serial:
        self.by_serial[(system, serial.upper())].append((system, name))
    self.systems.append(system)
    return count

  def lookup(self, crc, size, alt_crc=None, alt_size=None, systems=None):
    """Returns (system, name) for a CRC (optionally also the headerless CRC)."""
    cands = []
    for c, sz in ((crc, size), (alt_crc, alt_size)):
      if c is None:
        continue
      for sysname, name, dsize in self.by_crc.get(c, []):
        if systems and sysname not in systems:
          continue
        if dsize is not None and sz is not None and dsize != sz:
          continue
        cands.append((sysname, name))
    if not cands:
      return None
    prefs = systems or []
    cands.sort(key=lambda c: (prefs.index(c[0]) if c[0] in prefs else 99,
                              region_rank(c[1]), len(c[1]), c[1]))
    return cands[0]

  def lookup_crc(self, crc, ext):
    """Backwards compatible lookup by full-file CRC and extension."""
    hit = self.lookup(crc, None, systems=EXT_SYSTEMS.get(ext))
    return (hit[0], hit[1], None) if hit else None

  def art_fallback(self, stem, systems, gcode=None):
    """(system, name, how) used only for box art (never to rename), or None.
       Tries an exact file name match, then the GBA header game code (serial),
       then a loose title match."""
    cands = [c for c in self.by_norm.get(norm_name(stem), []) if c[0] in systems]
    if cands:
      cands.sort(key=lambda c: systems.index(c[0]))
      return cands[0] + ("filename",)
    if _HACK_RE.search(stem):
      return None
    if gcode:
      cands = [c for s in systems for c in self.by_serial.get((s, gcode.upper()), [])
               if not _BAD_TAGS.search(c[1])]
      if cands:
        cands.sort(key=lambda c: (systems.index(c[0]), 1 if "(rev" in c[1].lower() else 0,
                                  region_rank(c[1]), len(c[1]), c[1]))
        return cands[0] + ("game code %s" % gcode,)
    key = loose_name(stem)
    if not key:
      return None
    cands = [c for c in self.by_loose.get(key, [])
             if c[0] in systems and not _BAD_TAGS.search(c[1])]
    if not cands:
      return None
    hint = region_hint(stem)
    cands.sort(key=lambda c: (systems.index(c[0]),
                              0 if hint and hint in c[1].lower() else 1,
                              region_rank(c[1]), len(c[1]), c[1]))
    return cands[0] + ("fuzzy",)

  def art_alternates(self, system, name, limit=2):
    """Other releases of the same title (same system), to borrow box art from."""
    cands = [c for c in self.by_short.get(norm_name(short_name(name)), [])
             if c[0] == system and c[1] != name and not _BAD_TAGS.search(c[1])]
    cands.sort(key=lambda c: (region_rank(c[1]), len(c[1]), c[1]))
    return [c[1] for c in cands[:limit]]


def load_dats(cache_dir, offline=False, systems=None, logger=None, warner=None):
  logger = logger or log
  warner = warner or warn
  db = GameDB()
  datdir = os.path.join(cache_dir, "dats")
  os.makedirs(datdir, exist_ok=True)
  for system in (systems if systems is not None else SYSTEMS):
    path = os.path.join(datdir, system + ".dat")
    fresh = os.path.exists(path) and (time.time() - os.path.getmtime(path) < DAT_MAX_AGE)
    if not fresh and not offline:
      url = DAT_URL.format(urllib.parse.quote(system))
      try:
        data = http_get(url)
        if b"clrmamepro" not in data[:200] and b"game (" not in data:
          raise ValueError("downloaded file does not look like a clrmamepro DAT")
        write_atomic(path, data)
        logger("Downloaded DAT: %s" % system)
      except Exception as e:
        warner("could not download DAT '%s': %s%s" % (
               system, e, " (using cached copy)" if os.path.exists(path) else ""))
    if not os.path.exists(path):
      warner("no DAT available for '%s', those ROMs will not be identified" % system)
      continue
    with open(path, "r", encoding="utf-8", errors="replace") as fd:
      n = db.add_dat(system, fd.read())
    logger("Loaded %d entries from '%s' DAT" % (n, system))
  return db


# --- ROM hashing / sniffing ------------------------------------------------------

def crc32_file(path):
  crc = 0
  with open(path, "rb") as fd:
    while True:
      chunk = fd.read(1 << 20)
      if not chunk:
        break
      crc = zlib.crc32(chunk, crc)
  return crc & 0xFFFFFFFF


def header_skip(ext, head, size):
  """Size of a copier header that No-Intro does not include (0 if none)."""
  if head[:4] == b"NES\x1a":
    return 16
  if ext == ".pce" and size % 8192 == 512:
    return 512
  return 0


def hash_rom(path, ext, cancel=None, progress=None):
  """Returns dict(crc, alt, skip, b143) for a ROM file.
     alt is the CRC without the copier header (None if there is none).
     progress(nbytes) is called as data is read."""
  with open(path, "rb") as fd:
    head = fd.read(1 << 20)
    size = os.fstat(fd.fileno()).st_size
    skip = header_skip(ext, head, size)
    crc = zlib.crc32(head)
    alt = zlib.crc32(head[skip:]) if skip else None
    if progress:
      progress(len(head))
    while True:
      if cancel is not None and cancel.is_set():
        raise Cancelled()
      chunk = fd.read(4 << 20)
      if not chunk:
        break
      crc = zlib.crc32(chunk, crc)
      if alt is not None:
        alt = zlib.crc32(chunk, alt)
      if progress:
        progress(len(chunk))
  gcode = None
  if len(head) >= 0xC0 and head[4:4 + len(_GBA_LOGO)] == _GBA_LOGO:
    gc = head[0xAC:0xB0]
    if re.match(rb"^[A-Z0-9]{4}$", gc):
      gcode = gc.decode("ascii")
  return {"crc": crc & 0xFFFFFFFF, "alt": None if alt is None else alt & 0xFFFFFFFF,
          "skip": skip, "b143": head[0x143] if len(head) > 0x143 else None, "gcode": gcode}


def sniff_console(path):
  """Detects the console of a file with an unknown extension by its header."""
  try:
    with open(path, "rb") as fd:
      head = fd.read(0x150)
  except OSError:
    return None
  if head[:4] == b"NES\x1a":
    return "NES"
  if len(head) >= 0xB3 and head[4:4 + len(_GBA_LOGO)] == _GBA_LOGO and head[0xB2] == 0x96:
    return "GBA"
  if len(head) >= 0x144 and head[0x104:0x104 + len(_GB_LOGO)] == _GB_LOGO:
    return "GBC" if head[0x143] == 0xC0 else "GB"
  return None


class HashCache(object):
  """Thread-safe persistent CRC cache keyed by (path, size, mtime)."""

  def __init__(self, path):
    self.path = path
    self.lock = threading.Lock()
    self.data = {}
    self.dirty = False
    try:
      with open(path, "r", encoding="utf-8") as fd:
        d = json.load(fd)
      if isinstance(d, dict) and d.get("version") == 1:
        self.data = d.get("files", {})
    except (OSError, ValueError):
      pass

  def get(self, path, st):
    with self.lock:
      e = self.data.get(path)
    if e and len(e) >= 7 and e[0] == st.st_size and e[1] == st.st_mtime_ns:
      return {"crc": e[2], "alt": e[3], "skip": e[4], "b143": e[5], "gcode": e[6]}
    return None

  def put(self, path, st, h):
    with self.lock:
      self.data[path] = [st.st_size, st.st_mtime_ns, h["crc"], h["alt"], h["skip"], h["b143"],
                         h.get("gcode")]
      self.dirty = True

  def save(self):
    with self.lock:
      if not self.dirty:
        return
      files = {p: e for p, e in self.data.items() if os.path.exists(p)}
      payload = json.dumps({"version": 1, "files": files}, ensure_ascii=False)
      self.dirty = False
    try:
      write_atomic(self.path, payload.encode("utf-8"))
    except OSError as e:
      warn("cannot write hash cache %s: %s" % (self.path, e))


# --- Naming -------------------------------------------------------------------

def fat_sanitize(name):
  out = "".join("_" if (c in FAT_INVALID or ord(c) < 32) else c for c in name)
  out = out.strip(" ").rstrip(" .")
  return out or "_"


def sfw_stem(fname):
  """Mimics SuperFW util.c replace_extension(): strip the last extension."""
  i = fname.rfind(".")
  return fname[:i] if i >= 0 else fname


_NUMBERING_RE = re.compile(r"^\s*(?:\d{3,5}(?:\s+-\s+|\s*\.\s*|\s*_\s*|\s+)|\d{1,2}(?:\s+-\s+|\s*\.\s*))")


def clean_numbering(stem):
  """Strips collection numbering ("0145 - Name", "003 Name", "391.Name")."""
  m = _NUMBERING_RE.match(stem)
  if not m:
    return stem
  rest = stem[m.end():].strip()
  if not rest or rest[0] in "([" or not re.search(r"\w", rest):
    return stem
  return rest


def truncate_utf8(s, maxbytes):
  b = s.encode("utf-8")
  if len(b) <= maxbytes:
    return s
  s = b[:maxbytes].decode("utf-8", errors="ignore")
  sp = s.rfind(" ")
  if sp > len(s) - 24 and sp > 0:
    s = s[:sp]                  # cut at a word boundary
  # Do not leave a dangling, unclosed tag or separator behind.
  s = re.sub(r"\s*[(\[][^)\]]*$", "", s) or s
  return s.rstrip(" .-_,")


# --- Box art -----------------------------------------------------------------

def thumb_name(name):
  return "".join("_" if c in THUMB_INVALID else c for c in name)


_thumb_locks = collections.defaultdict(threading.Lock)
_thumb_locks_lock = threading.Lock()
_net_sem = threading.BoundedSemaphore(THUMB_MAX_CONCURRENCY)


def fetch_thumbnail(system, name, cache_dir, force, cancel=None):
  """Returns (png_bytes, kind, from_cache) or (None, reason, False). Thread-safe;
     at most THUMB_MAX_CONCURRENCY concurrent requests are made."""
  tname = thumb_name(name)
  reasons = []
  for kind in ("Named_Boxarts", "Named_Titles"):
    cdir = os.path.join(cache_dir, "thumbs", system, kind)
    cpath = os.path.join(cdir, fat_sanitize(tname) + ".png")
    miss = cpath + ".404"
    with _thumb_locks_lock:
      klock = _thumb_locks[cpath]
    with klock:
      if os.path.exists(cpath) and os.path.getsize(cpath) > 0:
        with open(cpath, "rb") as fd:
          return fd.read(), kind, True
      if os.path.exists(miss) and not force and time.time() - os.path.getmtime(miss) < DAT_MAX_AGE:
        reasons.append("%s: not found (cached)" % kind)
        continue
      url = THUMB_URL.format(*(urllib.parse.quote(x) for x in (system, kind, tname)))
      try:
        with _net_sem:
          data = http_get(url, cancel=cancel)
        if not data.startswith(b"\x89PNG"):
          raise ValueError("not a PNG")
        write_atomic(cpath, data)
        if os.path.exists(miss):
          os.unlink(miss)
        return data, kind, False
      except NotFound:
        reasons.append("%s: not found" % kind)
        try:
          os.makedirs(cdir, exist_ok=True)
          open(miss, "wb").close()
        except OSError:
          pass
      except Cancelled:
        raise
      except Exception as e:
        reasons.append("%s: %s" % (kind, e))
  return None, "; ".join(reasons), False


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


def _strip_png_ancillary(data):
  """Drops ancillary PNG chunks (except tRNS): some thumbnails have corrupt
     metadata chunks (eg. iCCP with a bad CRC) that Pillow refuses to open."""
  if not data.startswith(b"\x89PNG\r\n\x1a\n"):
    return data
  out, i = [data[:8]], 8
  while i + 8 <= len(data):
    n, t = struct.unpack(">I4s", data[i:i + 8])
    chunk = data[i:i + 12 + n]
    if t[:1].isupper() or t == b"tRNS":
      out.append(chunk)
    i += 12 + n
    if t == b"IEND":
      break
  return b"".join(out)


def _open_image(png_bytes):
  from PIL import Image
  try:
    img = Image.open(io.BytesIO(png_bytes))
    img.load()
  except Exception:
    img = Image.open(io.BytesIO(_strip_png_ancillary(png_bytes)))
    img.load()
  return img


def encode_art(png_bytes):
  """Converts an image to the SuperFW .img format. Returns (bytes, w, h, ncolors)."""
  from PIL import Image

  img = _open_image(png_bytes)
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
  """Strict check against docs/boxart-format.md. Returns (w, h, palette, pixels)."""
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


# --- Organizer (input collection -> fresh SuperFW SD layout) --------------------

class Options(object):
  def __init__(self, input_dir, output_dir, replace=False, replace_saves=False,
               art=True, saves=True, cheats=True, superfw_files=True,
               clean_names=True, dry_run=False, threads=None,
               cache_dir=DEFAULT_CACHE, exclude=("EMU",), report_path=None):
    self.input_dir = os.path.abspath(os.path.expanduser(input_dir))
    self.output_dir = os.path.abspath(os.path.expanduser(output_dir))
    self.replace = replace
    self.replace_saves = replace and replace_saves
    self.art = art
    self.saves = saves
    self.cheats = cheats
    self.superfw_files = superfw_files
    self.clean_names = clean_names
    self.dry_run = dry_run
    self.threads = threads or default_threads()
    self.cache_dir = os.path.abspath(os.path.expanduser(cache_dir))
    self.exclude = [e.strip().lower() for e in exclude if e.strip()]
    self.report_path = report_path


def default_threads():
  return min(16, (os.cpu_count() or 4) * 2)


class Reporter(object):
  """Progress sink. Methods may be called from any thread."""
  def on_log(self, level, msg):        # level: debug, info, warn, error
    pass

  def on_progress(self, snap):         # snap: see Organizer.snapshot()
    pass


PHASES = ["Scanning", "Identifying (CRC)", "Planning", "Copying", "Box art"]
_PHASE_WEIGHTS = [0.03, 0.40, 0.02, 0.35, 0.20]
LEVELS = {"debug": 0, "info": 1, "warn": 2, "error": 3}

# Counter keys (global and per console)
COUNTERS = [
  ("found", "ROMs found"), ("identified", "Identified"), ("unidentified", "Unidentified"),
  ("duplicates", "Duplicates in input"), ("already", "Already in output"),
  ("copied", "Copied"), ("replaced", "Replaced"), ("conflict", "Skipped (name taken)"),
  ("renamed", "Name collisions renamed"),
  ("saves_copied", "Saves copied"), ("saves_present", "Saves already in output"),
  ("saves_skipped", "Saves skipped (differ)"), ("cheats_copied", "Cheats copied"),
  ("superfw_copied", "SuperFW files copied"),
  ("art_created", "Art created"), ("art_present", "Art already present"),
  ("art_missing", "Art not available"), ("art_failed", "Art failed"),
  ("errors", "Errors"),
]


class RomFile(object):
  """A ROM file found in the input (or output)."""
  __slots__ = ("path", "rel", "fname", "ext", "console", "size", "mtime", "crc", "alt",
               "skip", "b143", "gcode", "system", "name", "orig_stem", "sniffed", "error")

  def __init__(self, path, rel, console, st, sniffed=False):
    self.path = path
    self.rel = rel
    self.fname = os.path.basename(path)
    self.ext = os.path.splitext(self.fname)[1].lower()
    self.console = console
    self.size = st.st_size
    self.mtime = st.st_mtime
    self.crc = self.alt = self.skip = self.b143 = self.gcode = None
    self.system = self.name = None
    self.orig_stem = sfw_stem(self.fname)
    self.sniffed = sniffed
    self.error = None

  @property
  def key(self):
    """Dedupe key: CRC/size of the ROM data (without copier header)."""
    if self.alt is not None:
      return (self.alt, self.size - self.skip)
    return (self.crc, self.size)


class Entry(object):
  """A unique ROM to be placed in the output."""
  def __init__(self, rom, dupes):
    self.rom = rom              # chosen copy
    self.dupes = dupes          # other copies (same content)
    self.console = rom.console
    self.final_fname = None
    self.target = None          # output path
    self.action = None          # copy, replace, already, conflict
    self.existing = None        # existing output path (already / conflict)
    self.saves = []             # [(src, dst)]
    self.cheat = None           # (src, dst)
    self.art_target = None
    self.art_src = None         # (system, name, how)


def _file_bytes(path):
  try:
    with open(path, "rb") as fd:
      return fd.read()
  except OSError:
    return path


def _all_not_found(reason):
  return bool(reason) and all("not found" in p for p in reason.split("; "))


class Organizer(object):
  def __init__(self, opts, reporter=None):
    self.o = opts
    self.rep = reporter or Reporter()
    self.cancel_event = threading.Event()
    self._lock = threading.Lock()
    self._log_lock = threading.Lock()
    self.counters = collections.Counter()
    self.per_console = collections.defaultdict(collections.Counter)
    self.phase = None
    self.phase_idx = -1
    self.phase_done = 0
    self.phase_total = 0
    self.item = ""
    self._last_emit = 0.0
    self.report_lines = []
    self.duplicates = []        # (kept rel, [dupe rels])
    self.collisions = []        # (rel, final name)
    self.unmatched_saves = []
    self.ignored = collections.Counter()
    self.timings = {}
    self.started = None
    self.cancelled = False
    self.report_file = None
    self.hcache = HashCache(os.path.join(opts.cache_dir, "crc-cache.json"))
    self._weights = list(_PHASE_WEIGHTS)
    if not opts.art:
      self._weights[4] = 0.0
    if opts.dry_run:
      self._weights[3] = 0.02
      self._weights[4] = min(self._weights[4], 0.02)
    tot = sum(self._weights)
    self._weights = [w / tot for w in self._weights]

  # -- reporting --

  def cancel(self):
    self.cancel_event.set()

  def cancelled_now(self):
    return self.cancel_event.is_set()

  def log(self, level, msg):
    with self._log_lock:
      self.report_lines.append("%-5s %s" % (level.upper(), msg))
    self.rep.on_log(level, msg)

  def inc(self, key, console=None, n=1):
    with self._lock:
      self.counters[key] += n
      if console:
        self.per_console[console][key] += n
    self._emit()

  def set_phase(self, idx, total=0):
    with self._lock:
      self.phase_idx = idx
      self.phase = PHASES[idx]
      self.phase_done = 0
      self.phase_total = total
      self.item = ""
    self.timings[PHASES[idx]] = time.time()
    self.log("info", "== %s ==" % PHASES[idx])
    self._emit(True)

  def set_total(self, total):
    with self._lock:
      self.phase_total = total
    self._emit(True)

  def advance(self, n=1, item=None):
    with self._lock:
      self.phase_done += n
      if item is not None:
        self.item = item
    self._emit()

  def set_item(self, item):
    with self._lock:
      self.item = item
    self._emit()

  def overall(self):
    if self.phase_idx < 0:
      return 0.0
    base = sum(self._weights[:self.phase_idx])
    frac = (self.phase_done / self.phase_total) if self.phase_total else 0.0
    return min(1.0, base + self._weights[self.phase_idx] * min(1.0, frac))

  def snapshot(self):
    with self._lock:
      return {
        "phase": self.phase, "phase_idx": self.phase_idx,
        "phase_done": self.phase_done, "phase_total": self.phase_total,
        "overall": self.overall(), "item": self.item,
        "counters": dict(self.counters),
        "elapsed": time.time() - self.started if self.started else 0.0,
      }

  def _emit(self, force=False):
    now = time.time()
    if not force and now - self._last_emit < 0.05:
      return
    self._last_emit = now
    self.rep.on_progress(self.snapshot())

  def _check(self):
    if self.cancel_event.is_set():
      raise Cancelled()

  # -- main --

  def run(self):
    """Runs the whole job. Returns True on success, False if cancelled."""
    self.started = time.time()
    o = self.o
    try:
      if not os.path.isdir(o.input_dir):
        raise OSError("input folder '%s' does not exist" % o.input_dir)
      if os.path.normcase(o.input_dir) == os.path.normcase(o.output_dir):
        raise OSError("input and output folders must be different")
      if (os.path.normcase(o.input_dir) + os.sep).startswith(os.path.normcase(o.output_dir) + os.sep):
        raise OSError("the input folder cannot be inside the output folder")
      if o.dry_run:
        self.log("info", "*** PREVIEW ONLY: nothing will be written to the output ***")
      self.log("info", "Input:  %s" % o.input_dir)
      self.log("info", "Output: %s" % o.output_dir)
      self.log("info", "Threads: %d" % o.threads)
      roms = self.scan()
      self.identify(roms)
      entries = self.plan(roms)
      self.copy(entries)      # (preview: only evaluates what would be copied)
      if o.art:
        self.art(entries)
    except Cancelled:
      self.cancelled = True
      self.log("warn", "Cancelled by user")
    except Exception as e:
      self.inc("errors")
      self.log("error", "Fatal: %s" % e)
      import traceback
      self.report_lines.append(traceback.format_exc())
      self.cancelled = None
    finally:
      self.hcache.save()
      self.timings["total"] = time.time() - self.started
      for line in self.summary_lines():
        self.log("info", line)
      self.write_report()
      with self._lock:
        if not self.cancelled and self.phase_idx >= 0:
          self.phase_done = self.phase_total
          self.phase_idx = len(PHASES) - 1
          self.phase_total = self.phase_done = 1
        self.item = ""
      self._emit(True)
    return self.cancelled is False

  # -- phase 1: scanning --

  def scan(self):
    o = self.o
    self.set_phase(0)
    roms = []
    self.save_index = collections.defaultdict(list)   # (dir, stem lower) -> [path]
    self.global_saves = collections.defaultdict(list)  # stem lower -> [path]
    self.cheats_index = {}                              # (dir, stem lower) -> path
    out_norm = os.path.normcase(o.output_dir) + os.sep
    root_save_dirs = {os.path.normcase(os.path.join(o.input_dir, d)).lower() for d in SAVE_DIRS}
    nfiles = 0
    for dirpath, dirnames, filenames in os.walk(o.input_dir):
      self._check()
      rel_dir = os.path.relpath(dirpath, o.input_dir)
      keep = []
      for d in sorted(dirnames):
        full = os.path.join(dirpath, d)
        dl = d.lower()
        if d.startswith(".") or dl in ("system volume information", "$recycle.bin") or \
           (rel_dir == "." and dl in o.exclude) or \
           (os.path.normcase(full) + os.sep).startswith(out_norm):
          if not d.startswith("."):
            self.log("info", "Skipping folder: %s" % os.path.relpath(full, o.input_dir))
          continue
        keep.append(d)
      dirnames[:] = keep
      is_save_dir = os.path.normcase(dirpath).lower() in root_save_dirs
      for fn in sorted(filenames):
        nfiles += 1
        full = os.path.join(dirpath, fn)
        rel = os.path.relpath(full, o.input_dir)
        ext = os.path.splitext(fn)[1].lower()
        stem_l = sfw_stem(fn).lower()
        if ext == ".sav":
          if is_save_dir:
            self.global_saves[stem_l].append(full)
          self.save_index[(dirpath, stem_l)].append(full)
          continue
        if ext == ".cht":
          self.cheats_index[(dirpath, stem_l)] = full
          continue
        try:
          st = os.stat(full)
        except OSError as e:
          self.log("warn", "Cannot stat %s: %s" % (rel, e))
          continue
        console = EXT_CONSOLE.get(ext)
        sniffed = False
        if console is None and ext not in (".sci", ".txt", ".exe", ".nds", ".patch", ".dat",
                                           ".mdb", ".srm", ".ini", ".jpg", ".png") \
           and 8192 <= st.st_size <= 64 << 20:
          console = sniff_console(full)
          sniffed = console is not None
          if sniffed:
            self.log("info", "Detected %s ROM by its header: %s" % (console, rel))
        if console is None:
          self.ignored[ext or "(no extension)"] += 1
          continue
        roms.append(RomFile(full, rel, console, st, sniffed))
        if len(roms) % 50 == 0:
          self.set_item(rel)
      self.counters["found"] = len(roms)
      self._emit()
    self.log("info", "Found %d ROM file(s) among %d files" % (len(roms), nfiles))
    if self.ignored:
      self.log("info", "Ignored files by extension: " + ", ".join(
        "%s: %d" % kv for kv in sorted(self.ignored.items(), key=lambda kv: -kv[1])))
    return roms

  # -- phase 2: identification --

  def _hash_one(self, rom, progress):
    try:
      st = os.stat(rom.path)
    except OSError as e:
      rom.error = str(e)
      return
    h = self.hcache.get(rom.path, st)
    if h is None:
      h = hash_rom(rom.path, rom.ext, self.cancel_event, progress)
      self.hcache.put(rom.path, st, h)
    else:
      progress(st.st_size)
    rom.crc, rom.alt, rom.skip, rom.b143 = h["crc"], h["alt"], h["skip"], h["b143"]
    rom.gcode = h.get("gcode")

  def _hash_all(self, roms, what):
    """Hashes ROM files in a thread pool (progress in bytes)."""
    def progress(n):
      self.advance(n)

    pool = ThreadPoolExecutor(max_workers=self.o.threads, thread_name_prefix="hash")
    try:
      futs = {pool.submit(self._hash_one, r, progress): r for r in roms}
      for f in as_completed(futs):
        r = futs[f]
        self._check()
        try:
          f.result()
        except Cancelled:
          raise
        except OSError as e:
          r.error = str(e)
        if r.error:
          self.inc("errors")
          self.log("error", "Cannot read %s: %s" % (r.rel, r.error))
        self.set_item("%s: %s" % (what, r.rel))
    finally:
      pool.shutdown(wait=True, cancel_futures=True)

  def identify(self, roms):
    o = self.o
    # Output index: existing console folders (recursively).
    self.out_dirs = {}          # console -> real output folder path
    self.out_files = []
    try:
      top = {n.lower(): n for n in os.listdir(o.output_dir)} if os.path.isdir(o.output_dir) else {}
    except OSError:
      top = {}
    for c in CONSOLES:
      real = top.get(c.folder.lower(), c.folder)
      self.out_dirs[c.folder] = os.path.join(o.output_dir, real)
    for c in CONSOLES:
      d = self.out_dirs[c.folder]
      if not os.path.isdir(d):
        continue
      for dirpath, dirnames, filenames in os.walk(d):
        for fn in filenames:
          full = os.path.join(dirpath, fn)
          if _PART_RE.search(fn):
            if not o.dry_run:
              try:
                os.unlink(full)
                self.log("info", "Removed stale temporary file %s" % os.path.relpath(full, o.output_dir))
              except OSError:
                pass
            continue
          ext = os.path.splitext(fn)[1].lower()
          if ext in EXT_CONSOLE:
            try:
              st = os.stat(full)
            except OSError:
              continue
            self.out_files.append(RomFile(full, os.path.relpath(full, o.output_dir), c.folder, st))

    total = sum(r.size for r in roms) + sum(r.size for r in self.out_files)
    self.set_phase(1, total)

    # DATs for the systems present.
    needed = []
    for r in roms:
      for s in CONSOLE_BY_FOLDER[r.console].systems:
        if s not in needed:
          needed.append(s)
    needed.sort(key=SYSTEMS.index)
    self.set_item("Loading No-Intro DATs...")
    self.db = load_dats(o.cache_dir, systems=needed,
                        logger=lambda m: self.log("info", m),
                        warner=lambda m: self.log("warn", m))
    self._check()

    t0 = time.time()
    self._hash_all(roms, "Hashing")
    if self.out_files:
      self.log("info", "Indexing %d ROM(s) already in the output" % len(self.out_files))
      self._hash_all(self.out_files, "Indexing output")
    self.log("info", "Hashing took %.1fs" % (time.time() - t0))

    for r in roms:
      if r.error:
        continue
      cons = CONSOLE_BY_FOLDER[r.console]
      hit = self.db.lookup(r.crc, r.size, r.alt, r.size - (r.skip or 0), cons.systems)
      if hit:
        r.system, r.name = hit
        r.console = SYSTEM_CONSOLE[r.system]
      elif r.console == "GB" and r.b143 == 0xC0:
        r.console = "GBC"      # GBC-only cartridge with a .gb extension

  # -- phase 3: planning --

  def _final_stem(self, rom):
    if rom.name:
      stem = rom.name
    else:
      fn = rom.fname
      ext = os.path.splitext(fn)[1]
      stem = fn[:-len(ext)] if ext and ext.lower() in EXT_CONSOLE else fn
      if rom.sniffed:
        cext = CONSOLE_BY_FOLDER[rom.console].ext[1:]
        if stem.lower().endswith(cext) and len(stem) > len(cext):
          stem = stem[:-len(cext)]          # "Game3nes" (missing dot)
      if self.o.clean_names:
        stem = clean_numbering(stem)
    return fat_sanitize(stem)

  def _final_ext(self, rom):
    if rom.name or rom.sniffed or rom.ext not in CONSOLE_BY_FOLDER[rom.console].exts:
      return CONSOLE_BY_FOLDER[rom.console].ext
    if rom.ext in (".sgb", ".dmg", ".agb", ".cgb"):   # not recognized by SuperFW
      return CONSOLE_BY_FOLDER[rom.console].ext
    return rom.ext

  def plan(self, roms):
    o = self.o
    self.set_phase(2, 4)
    roms = [r for r in roms if not r.error]
    self.rom_stems = {r.orig_stem.lower() for r in roms}

    # 1. Dedupe identical content within the input.
    groups = collections.OrderedDict()
    for r in sorted(roms, key=lambda r: r.rel.lower()):
      groups.setdefault(r.key, []).append(r)

    def has_save(r):
      st = r.orig_stem.lower()
      return bool(self.save_index.get((os.path.dirname(r.path), st)) or self.global_saves.get(st))

    def pref(r):
      stem = clean_numbering(r.orig_stem)
      wordy = len(re.findall(r"[^\W\d_]", stem))
      return (0 if r.name else 1, 0 if has_save(r) else 1, 0 if not r.sniffed else 1,
              -min(wordy, 40), r.rel.lower())

    entries = []
    for key, grp in groups.items():
      grp.sort(key=pref)
      e = Entry(grp[0], grp[1:])
      entries.append(e)
      self.per_console[e.console]["found"] += len(grp)
      if grp[1:]:
        self.inc("duplicates", e.console, len(grp) - 1)
        self.duplicates.append((grp[0].rel, [d.rel for d in grp[1:]]))
        self.log("info", "Duplicate content: %s  (copied once from: %s)" % (
          ", ".join(d.rel for d in grp[1:]), grp[0].rel))
      self.inc("identified" if grp[0].name else "unidentified", e.console)
    self.advance(1)

    # 2. Allocate final names. Identified ROMs first, so that they keep the
    #    clean official name. SuperFW keys saves/states by stem only (and FAT is
    #    case-insensitive), so stems must be unique across all consoles.
    entries.sort(key=lambda e: (0 if e.rom.name else 1, e.console, e.rom.rel.lower()))
    taken = {}          # lowercase stem -> console
    for e in entries:
      stem, ext = self._final_stem(e.rom), self._final_ext(e.rom)
      maxb = MAX_NAME_BYTES - len(ext.encode()) - 6
      if len(stem.encode("utf-8")) > maxb:
        nstem = truncate_utf8(stem, maxb)
        self.log("warn", "Name too long for SuperFW, truncated: %s -> %s%s" % (stem, nstem, ext))
        stem = nstem
      final, n = stem, 2
      other = taken.get(stem.lower())
      if other and other != e.console:
        # Same title on another console: SuperFW would share the save file.
        final = base = "%s [%s]" % (stem, e.console)
      else:
        base = stem
      while final.lower() in taken:
        final = "%s (%d)" % (base, n)
        n += 1
      taken[final.lower()] = e.console
      if final != stem:
        self.inc("renamed", e.console)
        self.collisions.append((e.rom.rel, final + ext))
        why = ("a %s ROM has the same name (saves are keyed by name)" % other
               if other and other != e.console else "another ROM has the same name")
        self.log("warn", "Name collision: %s would be named '%s%s' but %s; using '%s%s'" % (
          e.rom.rel, stem, ext, why, final, ext))
      e.final_fname = final + ext
    self.advance(1)

    # 3. Compare with the output.
    out_by_key = collections.defaultdict(dict)    # console -> key -> path
    for r in self.out_files:
      if not r.error and r.crc is not None:
        out_by_key[r.console].setdefault(r.key, r.path)
    listings = {}

    def lookup_out(path):
      d, f = os.path.split(path)
      if d not in listings:
        try:
          listings[d] = {n.lower(): n for n in os.listdir(d)}
        except OSError:
          listings[d] = {}
      real = listings[d].get(f.lower())
      return os.path.join(d, real) if real else None

    for e in entries:
      r = e.rom
      cdir = self.out_dirs[e.console]
      e.target = os.path.join(cdir, e.final_fname)
      existing = out_by_key[e.console].get(r.key)
      if existing:
        e.action, e.existing = "already", existing
        self.inc("already", e.console)
        self.log("debug", "Already in output: %s == %s" % (r.rel, os.path.relpath(existing, o.output_dir)))
      else:
        cur = lookup_out(e.target)
        if cur:
          if o.replace:
            e.action, e.existing = "replace", cur
            self.log("info", "Will replace %s (different content) with %s" % (
              os.path.relpath(cur, o.output_dir), r.rel))
          else:
            e.action, e.existing = "conflict", cur
            self.inc("conflict", e.console)
            self.log("warn", "Skipped %s: %s already exists with different content "
                     "(enable Replace to overwrite)" % (r.rel, os.path.relpath(cur, o.output_dir)))
        else:
          e.action = "copy"
        if e.action in ("copy", "replace"):
          self.log("debug", "%s -> %s%s" % (r.rel, os.path.relpath(e.target, o.output_dir),
                                            "  [%s]" % r.name if r.name else "  [unidentified]"))
    self.advance(1)

    # 4. Saves, cheats and art targets.
    used_saves = set()
    clean_stem_count = collections.Counter()
    for e in entries:
      for r in [e.rom] + e.dupes:
        clean_stem_count[clean_numbering(r.orig_stem).lower()] += 1
    for k in list(clean_stem_count):
      if k in self.rom_stems:
        clean_stem_count[k] += 1     # a ROM really has this name: not a fallback
    savedir = os.path.join(o.output_dir, self._real_top("SAVEGAME"))
    artdir = os.path.join(o.output_dir, ART_DIR)
    for e in entries:
      if e.action == "conflict":
        continue
      placed = e.existing if e.action == "already" else e.target
      pstem = sfw_stem(os.path.basename(placed))
      copies = [e.rom] + e.dupes
      if o.saves:
        cands = []
        for r in copies:
          st = r.orig_stem.lower()
          for p in self.save_index.get((os.path.dirname(r.path), st), []) + self.global_saves.get(st, []):
            if p not in cands:
              cands.append(p)
        if not cands:
          # Fallback: a save named after the ROM without its collection numbering
          # (eg. "Gradius 2 (J).sav" for "0123 Gradius 2 (J).nes"), if unambiguous.
          for r in copies:
            cs = clean_numbering(r.orig_stem).lower()
            if cs != r.orig_stem.lower() and clean_stem_count[cs] == 1:
              for p in self.save_index.get((os.path.dirname(r.path), cs), []) + self.global_saves.get(cs, []):
                if p not in cands:
                  cands.append(p)
        used_saves.update(cands)
        if cands:
          cands.sort(key=lambda p: -os.path.getmtime(p))
          e.saves = [(cands[0], os.path.join(savedir, pstem + ".sav"))]
          if len(cands) > 1 and len({_file_bytes(p) for p in cands}) > 1:
            self.log("warn", "Several saves for %s; using the newest: %s (ignored: %s)" % (
              os.path.basename(placed), os.path.relpath(cands[0], o.input_dir),
              ", ".join(os.path.relpath(p, o.input_dir) for p in cands[1:])))
      if o.cheats:
        for r in copies:
          c = self.cheats_index.get((os.path.dirname(r.path), r.orig_stem.lower()))
          if c:
            e.cheat = (c, os.path.join(os.path.dirname(placed), pstem + ".cht"))
            break
      e.art_target = os.path.join(artdir, os.path.basename(placed) + ".img")
      if e.rom.name:
        e.art_src = (e.rom.system, e.rom.name, "crc")
      else:
        stem = clean_numbering(e.rom.orig_stem)
        e.art_src = self.db.art_fallback(stem, CONSOLE_BY_FOLDER[e.console].systems,
                                         e.rom.gcode if e.console == "GBA" else None)
    if o.saves:
      allsaves = {p for v in self.save_index.values() for p in v}
      self.unmatched_saves = sorted(os.path.relpath(p, o.input_dir) for p in allsaves - used_saves)
      if self.unmatched_saves:
        self.log("warn", "%d save file(s) do not match any ROM and were not copied: %s" % (
          len(self.unmatched_saves), ", ".join(self.unmatched_saves)))
    self.advance(1)

    n = collections.Counter(e.action for e in entries)
    self.log("info", "Plan: %d unique ROM(s): %d to copy, %d to replace, %d already in output, "
             "%d skipped (name taken)" % (len(entries), n["copy"], n["replace"], n["already"], n["conflict"]))
    return entries

  def _real_top(self, name):
    try:
      for n in os.listdir(self.o.output_dir):
        if n.lower() == name.lower():
          return n
    except OSError:
      pass
    return name

  # -- phase 4: copying --

  def _copy_file(self, src, dst, existing=None):
    """Copies src to dst atomically (temp file + rename). If 'existing' is the
       (case-insensitively equal) file being replaced, it is replaced."""
    d = os.path.dirname(dst)
    os.makedirs(d, exist_ok=True)
    tmp = tmp_name(dst)
    try:
      with open(src, "rb") as fi, open(tmp, "wb") as fo:
        while True:
          if self.cancel_event.is_set():
            raise Cancelled()
          chunk = fi.read(1 << 20)
          if not chunk:
            break
          fo.write(chunk)
          self.advance(len(chunk))
      try:
        st = os.stat(src)
        os.utime(tmp, ns=(st.st_atime_ns, st.st_mtime_ns))
      except OSError:
        pass
      if existing and os.path.basename(existing) != os.path.basename(dst):
        os.replace(tmp, existing)       # same name modulo case
        os.rename(existing, dst)
      else:
        os.replace(tmp, dst)
    except BaseException:
      try:
        os.unlink(tmp)
      except OSError:
        pass
      raise

  def _place_small(self, src, dst, replace, kind, console=None):
    """Copies a save/cheat/support file. Returns 'copied', 'present' or 'skipped'."""
    d, f = os.path.split(dst)
    cur = None
    try:
      for n in os.listdir(d):
        if n.lower() == f.lower():
          cur = os.path.join(d, n)
          break
    except OSError:
      pass
    if cur:
      try:
        with open(src, "rb") as a, open(cur, "rb") as b:
          same = a.read() == b.read()
      except OSError:
        same = False
      if same:
        self.advance(os.path.getsize(src))
        return "present"
      if not replace:
        self.advance(os.path.getsize(src))
        self.log("warn", "Skipped %s %s: %s exists with different content" % (
          kind, os.path.relpath(src, self.o.input_dir), os.path.relpath(cur, self.o.output_dir)))
        return "skipped"
    if self.o.dry_run:
      self.advance(os.path.getsize(src))
    else:
      self._copy_file(src, dst, cur)
    self.log("debug", "%s%s %s -> %s" % ("Would copy " if self.o.dry_run else "", kind,
                                         os.path.relpath(src, self.o.input_dir),
                                         os.path.relpath(dst, self.o.output_dir)))
    return "copied"

  def _copy_entry(self, e):
    o = self.o
    if e.action in ("copy", "replace"):
      self.set_item("Copying %s" % e.final_fname)
      if o.dry_run:
        self.advance(e.rom.size)
      else:
        self._copy_file(e.rom.path, e.target, e.existing if e.action == "replace" else None)
        r = e.rom    # Same content: prime the CRC cache so re-runs need not re-hash it.
        try:
          self.hcache.put(e.target, os.stat(e.target), {"crc": r.crc, "alt": r.alt, "skip": r.skip,
                                                        "b143": r.b143, "gcode": r.gcode})
        except OSError:
          pass
      self.inc("copied" if e.action == "copy" else "replaced", e.console)
    for src, dst in e.saves:
      self._check()
      res = self._place_small(src, dst, o.replace_saves, "save")
      self.inc({"copied": "saves_copied", "present": "saves_present",
                "skipped": "saves_skipped"}[res], e.console)
    if e.cheat:
      res = self._place_small(e.cheat[0], e.cheat[1], o.replace, "cheats")
      if res == "copied":
        self.inc("cheats_copied", e.console)

  def copy(self, entries):
    o = self.o
    todo = [e for e in entries if e.action != "conflict"]
    total = sum(e.rom.size for e in todo if e.action in ("copy", "replace"))
    total += sum(os.path.getsize(s) for e in todo for s, _ in e.saves)
    total += sum(os.path.getsize(e.cheat[0]) for e in todo if e.cheat)
    sfw_files = []
    if o.superfw_files:
      for sub in ("emulators", "cheats"):
        sdir = os.path.join(o.input_dir, SUPERFW_DIR, sub)
        if os.path.isdir(sdir):
          for dp, dn, fns in os.walk(sdir):
            for fn in fns:
              src = os.path.join(dp, fn)
              rel = os.path.relpath(src, os.path.join(o.input_dir, SUPERFW_DIR))
              sfw_files.append((src, os.path.join(o.output_dir, self._real_top(SUPERFW_DIR), rel)))
      total += sum(os.path.getsize(s) for s, _ in sfw_files)
    self.set_phase(3, max(1, total))
    # Copying to SD cards is fastest with few concurrent writers.
    workers = max(1, min(o.threads, 4))
    pool = ThreadPoolExecutor(max_workers=workers, thread_name_prefix="copy")
    try:
      futs = {}
      for e in todo:
        futs[pool.submit(self._copy_entry, e)] = e.rom.rel
      for src, dst in sfw_files:
        futs[pool.submit(self._copy_sfw, src, dst)] = os.path.relpath(src, o.input_dir)
      for f in as_completed(futs):
        self._check()
        try:
          f.result()
        except Cancelled:
          raise
        except OSError as ex:
          self.inc("errors")
          self.log("error", "Copy failed for %s: %s" % (futs[f], ex))
    finally:
      if self.cancel_event.is_set():
        for f in futs:
          f.cancel()
      pool.shutdown(wait=True, cancel_futures=True)

  def _copy_sfw(self, src, dst):
    self._check()
    res = self._place_small(src, dst, self.o.replace, "SuperFW file")
    if res == "copied":
      self.inc("superfw_copied")

  # -- phase 5: box art --

  def _art_one(self, e):
    o = self.o
    if self.cancel_event.is_set():
      raise Cancelled()
    name = os.path.basename(e.art_target)
    if e.art_src is None:
      self.inc("art_missing", e.console)
      self.log("debug", "No art: %s (not identified)" % name)
      self.advance(1)
      return
    exists = os.path.exists(e.art_target) and art_is_valid(e.art_target)
    if exists and not o.replace:
      self.inc("art_present", e.console)
      self.advance(1)
      return
    system, gname, how = e.art_src
    if o.dry_run:
      self.log("debug", "Would create art %s from '%s' (%s)" % (name, gname, how))
      self.inc("art_created", e.console)
      self.advance(1)
      return
    self.set_item("Box art: %s" % gname)
    png, kind, cached = fetch_thumbnail(system, gname, o.cache_dir, False, self.cancel_event)
    if png is None and _all_not_found(kind):
      for alt in self.db.art_alternates(system, gname):
        apng, akind, acached = fetch_thumbnail(system, alt, o.cache_dir, False, self.cancel_event)
        if apng is not None:
          self.log("debug", "No art for '%s', using the art of '%s'" % (gname, alt))
          png, kind, cached = apng, akind, acached
          break
    if png is None:
      if _all_not_found(kind):
        self.inc("art_missing", e.console)
        self.log("debug", "No art available for '%s' (%s)" % (gname, how))
      else:
        self.inc("art_failed", e.console)
        self.log("warn", "Art download failed for '%s': %s" % (gname, kind))
      self.advance(1)
      return
    try:
      data, w, h, n = encode_art(png)
      write_atomic(e.art_target, data)
      self.inc("art_created", e.console)
      self.log("debug", "Art %s: %s%s %dx%d, %d colors" % (
        name, kind, " (cached)" if cached else "", w, h, n))
    except Exception as ex:
      self.inc("art_failed", e.console)
      self.log("warn", "Art conversion failed for '%s': %s" % (gname, ex))
    self.advance(1)

  def art(self, entries):
    o = self.o
    todo = [e for e in entries if e.action != "conflict" and e.art_target]
    self.set_phase(4, max(1, len(todo)))
    if not o.dry_run:
      try:
        import PIL  # noqa: F401
      except ImportError:
        self.log("error", "Pillow is not installed: box art skipped (pip install Pillow)")
        return
      os.makedirs(os.path.join(o.output_dir, ART_DIR), exist_ok=True)
    pool = ThreadPoolExecutor(max_workers=o.threads, thread_name_prefix="art")
    try:
      futs = {pool.submit(self._art_one, e): e for e in todo}
      for f in as_completed(futs):
        self._check()
        try:
          f.result()
        except Cancelled:
          raise
        except Exception as ex:
          self.inc("art_failed", futs[f].console)
          self.log("warn", "Art failed for %s: %s" % (futs[f].rom.rel, ex))
    finally:
      pool.shutdown(wait=True, cancel_futures=True)

  # -- summary / report --

  def summary_lines(self):
    c = self.counters
    t = self.timings.get("total", time.time() - (self.started or time.time()))
    status = "CANCELLED" if self.cancelled else ("FAILED" if self.cancelled is None else "done")
    lines = ["", "== Summary (%s%s, %.1fs) ==" % ("preview, " if self.o.dry_run else "", status, t)]
    lines.append("ROMs found: %d, unique: %d, identified: %d, unidentified: %d, duplicates in input: %d" % (
      c["found"], c["identified"] + c["unidentified"], c["identified"], c["unidentified"], c["duplicates"]))
    verb = "to copy" if self.o.dry_run else "copied"
    lines.append("Already in output: %d, %s: %d, replaced: %d, skipped (name taken): %d, "
                 "name collisions renamed: %d" % (c["already"], verb, c["copied"], c["replaced"],
                                                  c["conflict"], c["renamed"]))
    lines.append("Saves copied: %d (already present: %d, skipped: %d), cheats copied: %d, "
                 "SuperFW files copied: %d" % (c["saves_copied"], c["saves_present"], c["saves_skipped"],
                                               c["cheats_copied"], c["superfw_copied"]))
    if self.o.art:
      lines.append("Art created: %d, already present: %d, not available: %d, failed: %d" % (
        c["art_created"], c["art_present"], c["art_missing"], c["art_failed"]))
    if c["errors"]:
      lines.append("Errors: %d" % c["errors"])
    cols = [("found", "Found"), ("identified", "Ident"), ("unidentified", "Unid"),
            ("duplicates", "Dupes"), ("already", "InOut"), ("copied", "Copied"),
            ("conflict", "Taken"), ("art_created", "ArtNew"), ("art_present", "ArtHad"),
            ("art_missing", "ArtNone"), ("art_failed", "ArtFail")]
    lines.append("")
    lines.append("%-6s" % "" + "".join("%8s" % h for _, h in cols))
    for cons in CONSOLES:
      pc = self.per_console.get(cons.folder)
      if not pc or not pc["found"] and not pc["identified"] and not pc["unidentified"]:
        continue
      lines.append("%-6s" % cons.folder + "".join("%8d" % pc[k] for k, _ in cols))
    return lines

  def write_report(self):
    o = self.o
    path = o.report_path
    if not path:
      path = os.path.join(o.cache_dir, "reports",
                          time.strftime("report-%Y%m%d-%H%M%S.txt"))
    try:
      body = ["SuperFW ROM manager report - %s" % time.strftime("%Y-%m-%d %H:%M:%S"),
              "Input:  %s" % o.input_dir, "Output: %s" % o.output_dir,
              "Options: replace=%s replace_saves=%s art=%s saves=%s cheats=%s superfw_files=%s "
              "clean_names=%s dry_run=%s threads=%d" % (
                o.replace, o.replace_saves, o.art, o.saves, o.cheats, o.superfw_files,
                o.clean_names, o.dry_run, o.threads), ""]
      with self._log_lock:
        body += self.report_lines
      if self.duplicates:
        body += ["", "== Duplicates in input (%d groups) ==" % len(self.duplicates)]
        for kept, d in self.duplicates:
          body.append("%s\n    same content as: %s" % (kept, "; ".join(d)))
      if self.collisions:
        body += ["", "== Name collisions (renamed) =="]
        body += ["%s -> %s" % c for c in self.collisions]
      write_atomic(path, ("\n".join(body) + "\n").encode("utf-8", errors="replace"))
      self.report_file = path
      self.rep.on_log("info", "Report written to %s" % path)
    except OSError as e:
      self.rep.on_log("warn", "Cannot write report %s: %s" % (path, e))
