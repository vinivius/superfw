#!/usr/bin/env python3
#
# SuperFW SD card ROM scraper.
#
# Two modes:
#
# 1. In-place (sd_root): identifies GBA/GB/GBC/NES/... ROMs on an SD card (CRC32
#    lookup against the No-Intro DATs shipped by libretro-database), renames them
#    to their official No-Intro names (together with all the per-ROM files SuperFW
#    keeps: saves, save backups, savestates, cheats, patches, per-ROM config,
#    recent list entries...) and generates box-art thumbnails in the SuperFW .img
#    format.
#
# 2. Organize (--input/--output): builds a fresh SuperFW SD card layout from a
#    ROM collection: one folder per console, official names, saves copied to
#    SAVEGAME/, cheats copied along, box art generated. Same engine as the GUI
#    (rom-manager-gui.py).
#
# Usage:
#   rom-scraper.py <sd_root> [--dry-run] [--no-rename] [--no-art] [--force-art] [--cache DIR]
#   rom-scraper.py --input DIR --output DIR [--replace] [--replace-saves] [--no-art]
#                  [--threads N] [--dry-run] [...]
#   rom-scraper.py --preview file.img out.png [--scale N]
#
# Requires Python 3.7+ and Pillow (only needed for art generation / preview).
# The shared logic lives in superfw_romlib.py (next to this script).

import argparse
import collections
import os
import re
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from superfw_romlib import (  # noqa: E402
  ART_DIR, CONFIG_DIR, DEFAULT_CACHE, EXT_SYSTEMS, MAX_FN_LEN, PATCHDB_DIR,
  PENDING_SAVE_FILE, RECENT_FILE, ROM_EXTS, SAVE_DIRS, STATE_DIRS, SUPERFW_DIR,
  LEVELS, Options, Organizer, Reporter,
  art_is_valid, default_threads, encode_art, fat_sanitize, fetch_thumbnail,
  hash_rom, load_dats, log, preview, sfw_stem, warn, write_atomic)


# --- ROM scanning / identification (in-place mode) ---------------------------------

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
  ext = rom.ext.lower()
  h = hash_rom(rom.path, ext)
  rom.crc = h["crc"]
  size = os.path.getsize(rom.path)
  prefs = EXT_SYSTEMS.get(ext, [])
  hit = db.lookup(h["crc"], size, h["alt"], size - h["skip"], prefs)
  if hit:
    rom.system, rom.name = hit
    rom.match = "crc"
    return
  # Filename fallback (exact normalized match: rename), or fuzzy match on the
  # short title, used only to find box art, never to rename (the dump might be
  # a hack/beta/...).
  fb = db.art_fallback(os.path.splitext(rom.fname)[0], prefs)
  if fb:
    rom.system, rom.name, rom.match = fb


# --- Renaming ----------------------------------------------------------------

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


# --- Organize mode ----------------------------------------------------------------

class TextReporter(Reporter):
  def __init__(self, verbose):
    self.verbose = verbose
    self.tty = sys.stderr.isatty()
    self.lock = threading.Lock()
    self.last_line = 0.0
    self.last_phase = None
    self.width = 0

  def _clear(self):
    if self.tty and self.width:
      sys.stderr.write("\r" + " " * self.width + "\r")
      self.width = 0

  def on_log(self, level, msg):
    if LEVELS[level] < (0 if self.verbose else 1):
      return
    with self.lock:
      self._clear()
      if level in ("warn", "error"):
        print("%s: %s" % (level.upper().replace("WARN", "WARNING"), msg), flush=True)
      else:
        print(msg, flush=True)

  def on_progress(self, snap):
    now = time.time()
    with self.lock:
      if not self.tty and now - self.last_line < 5 and snap["phase"] == self.last_phase:
        return
      self.last_line = now
      self.last_phase = snap["phase"]
      c = snap["counters"]
      line = "[%3d%%] %s %s/%s | found %d ident %d unid %d dup %d in-out %d copied %d art %d" % (
        int(snap["overall"] * 100), snap["phase"] or "", _fmt(snap["phase_done"], snap["phase_idx"]),
        _fmt(snap["phase_total"], snap["phase_idx"]), c.get("found", 0), c.get("identified", 0),
        c.get("unidentified", 0), c.get("duplicates", 0), c.get("already", 0),
        c.get("copied", 0), c.get("art_created", 0))
      if self.tty:
        cols = 160
        try:
          cols = os.get_terminal_size(sys.stderr.fileno()).columns
        except OSError:
          pass
        line = line[:cols - 1]
        sys.stderr.write("\r" + line.ljust(self.width))
        self.width = len(line)
      else:
        sys.stderr.write(line + "\n")
      sys.stderr.flush()

  def done(self):
    with self.lock:
      self._clear()


def _fmt(n, phase_idx):
  # Hashing and copying progress is in bytes.
  if phase_idx in (1, 3):
    return "%.0fM" % (n / 1048576.0)
  return str(n)


def organize(args):
  if args.replace_saves and not args.replace:
    print("ERROR: --replace-saves requires --replace", file=sys.stderr)
    return 1
  opts = Options(args.input, args.output, replace=args.replace, replace_saves=args.replace_saves,
                 art=not args.no_art, saves=not args.no_saves, cheats=not args.no_cheats,
                 superfw_files=not args.no_superfw_files, clean_names=not args.keep_numbering,
                 dry_run=args.dry_run, threads=max(1, args.threads), cache_dir=args.cache,
                 exclude=args.exclude if args.exclude is not None else ["EMU"],
                 report_path=args.report)
  rep = TextReporter(args.verbose)
  org = Organizer(opts, rep)
  result = {}
  th = threading.Thread(target=lambda: result.setdefault("ok", org.run()), name="organizer")
  th.start()
  try:
    while th.is_alive():
      th.join(0.2)
  except KeyboardInterrupt:
    org.cancel()
    th.join()
  rep.done()
  if org.cancelled:
    return 130
  return 0 if result.get("ok") and not org.counters["errors"] else 1


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
    description="Identify, rename and fetch box art for ROMs on a SuperFW SD card (in place), "
                "or build a fresh SuperFW SD layout from a ROM collection (--input/--output).",
    epilog="Preview mode: rom-scraper.py --preview file.img out.png [--scale N]")
  ap.add_argument("sd_root", nargs="?", help="SD card root directory (in-place mode)")
  ap.add_argument("--dry-run", action="store_true", help="show what would be done, change nothing")
  ap.add_argument("--no-rename", action="store_true", help="in-place: do not rename ROMs (or their files)")
  ap.add_argument("--no-art", action="store_true", help="do not generate box art")
  ap.add_argument("--force-art", action="store_true", help="in-place: regenerate existing art files")
  ap.add_argument("--cache", default=DEFAULT_CACHE,
                  help="cache dir for DATs, thumbnails and CRCs (default: %(default)s)")
  og = ap.add_argument_group("organize mode (copy a collection into a new SuperFW SD layout)")
  og.add_argument("--input", metavar="DIR", help="ROM collection to read (never modified)")
  og.add_argument("--output", metavar="DIR", help="output folder / SD card root")
  og.add_argument("--replace", action="store_true",
                  help="replace existing output files with different content (ROMs, cheats, art)")
  og.add_argument("--replace-saves", action="store_true",
                  help="also replace existing save files (requires --replace)")
  og.add_argument("--threads", type=int, default=default_threads(),
                  help="worker threads (default: %(default)s)")
  og.add_argument("--no-saves", action="store_true", help="do not copy save files")
  og.add_argument("--no-cheats", action="store_true", help="do not copy per-ROM .cht files")
  og.add_argument("--no-superfw-files", action="store_true",
                  help="do not copy .superfw/emulators and .superfw/cheats from the input")
  og.add_argument("--keep-numbering", action="store_true",
                  help="do not strip leading numbering from unidentified ROM names")
  og.add_argument("--exclude", action="append", metavar="NAME", default=None,
                  help="top-level input folder to skip (repeatable; default: EMU)")
  og.add_argument("--report", metavar="FILE", help="report file (default: in the cache dir)")
  og.add_argument("-v", "--verbose", action="store_true", help="log every file")
  args = ap.parse_args()

  if args.input or args.output:
    if not (args.input and args.output) or args.sd_root:
      ap.error("--input and --output must be used together (and without sd_root)")
    return organize(args)
  if not args.sd_root:
    ap.error("either sd_root or --input/--output is required")

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

  paths = scan_roms(sd)
  systems = []
  for p in paths:
    for sname in EXT_SYSTEMS[os.path.splitext(p)[1].lower()]:
      if sname not in systems:
        systems.append(sname)
  db = load_dats(cache, systems=systems)
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
      png, kind, _ = fetch_thumbnail(rom.system, rom.name, cache, args.force_art)
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
