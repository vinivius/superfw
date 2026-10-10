/*
 * Copyright (C) 2026 David Guillen Fandos <david@davidgf.net>
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include "recent.h"

#include "common.h"
#include "compiler.h"
#include "gbahw.h"
#include "util.h"
#include "fatfs/ff.h"
#include "fileutil.h"

#pragma GCC optimize ("Os")

NOINLINE bool recent_flush(const t_rentry *rentries, unsigned rcount) {
  WRITE_LOG("Flushing recently played games (%d entries)", rcount);

  // Flush to disk, to a temporary file that replaces the list once written
  // whole (a cut list would be misread). FatFs buffers the small writes.
  FIL fo;
  if (!superfw_file_open(&fo, NULL, RECENT_FILEPATH ".tmp", FA_CREATE_ALWAYS))
    return false;
  bool ok = true;
  for (unsigned i = 0; ok && i < rcount; i++) {
    const char *fn = rentries[i].fpath;
    ok = (!(rentries[i].flags & FLAG_RECENT_NOR) || write_all(&fo, "nor:", 4)) &&
         write_all(&fo, fn, strlen(fn)) && write_all(&fo, "\n", 1);
  }
  // The data reaches the card when it's closed.
  ok = FR_OK == f_close(&fo) && ok;
  return file_replace(RECENT_FILEPATH ".tmp", RECENT_FILEPATH, ok);
}

// The list is in the cart's SDRAM and saved as it is there: its writes are
// checked. The bytes of an entry with a path of len chars (whole words).
#define RENTRY_BYTES(len)  (offsetof(t_rentry, fpath) + (((len) + 4) & ~3U))

// Moves n entries from src to dst (overlapping too), checked.
static bool rentry_move(t_rentry *r, unsigned dst, unsigned src, unsigned n) {
  for (unsigned k = 0; k < n; k++) {
    const unsigned i = dst > src ? n - 1 - k : k;
    if (!memcpy32_checked(&r[dst + i], &r[src + i], sizeof(t_rentry)))
      return false;
  }
  return true;
}

NOINLINE int insert_recent_fn(t_rentry *rentries, unsigned rcount, const char *fn, unsigned flags) {
  WRITE_LOG("Adding/bumping recently played game: '%s' [%x]", fn, flags);

  t_rentry e;
  unsigned i = 0, len;
  while (i < rcount && (rentries[i].flags != flags || strcmp(rentries[i].fpath, fn)))
    i++;
  if (i < rcount) {
    // Found a matching file: it moves to position 0.
    memcpy32(&e, &rentries[i], sizeof(e));
    len = strlen(e.fpath);
  }
  else {
    // Not in the list: it goes first, the oldest entry falls off the end when
    // the list is full.
    i = MIN(rcount, RECENT_MAXFN_CNT - 1);
    rcount = MIN(rcount + 1, RECENT_MAXFN_CNT);
    e.flags = flags;
    e.fname_offset = file_basename(fn) - fn;
    len = strlen(fn);
    memcpy(e.fpath, fn, len + 1);
  }
  // The ones before it move down one.
  return rentry_move(rentries, 1, 0, i) && memcpy32_checked(&rentries[0], &e, RENTRY_BYTES(len)) ? (int)rcount : -1;
}

NOINLINE int delete_recent(t_rentry *rentries, unsigned rcount, unsigned entry_num) {
  if (entry_num >= rcount)
    return rcount;
  // The ones after it move up one.
  return rentry_move(rentries, entry_num, entry_num + 1, rcount - entry_num - 1) ? (int)rcount - 1 : -1;
}

typedef struct {
  t_rentry *rentries;
  unsigned cnt;
  bool error;               // An entry couldn't be written (SDRAM)
} t_recent_read;

// A line of the list: a path (or "nor:" and a path).
static bool recent_line(char *line, unsigned len, void *usr) {
  t_recent_read *rd = (t_recent_read*)usr;
  if (!line)
    return true;            // Too long to be a path
  bool nor = len >= 4 && !memcmp(line, "nor:", 4);
  const char *path = nor ? &line[4] : line;
  unsigned plen = nor ? len - 4 : len;
  // Skip empty lines, and paths that don't fit an entry.
  if (plen && plen < MAX_FN_LEN) {
    t_rentry e;
    e.flags = nor ? FLAG_RECENT_NOR : 0;
    memcpy(e.fpath, path, plen);
    e.fpath[plen] = 0;
    e.fname_offset = file_basename(e.fpath) - e.fpath;
    rd->error = !memcpy32_checked(&rd->rentries[rd->cnt++], &e, RENTRY_BYTES(plen));
  }
  return !rd->error && rd->cnt < RECENT_MAXFN_CNT;
}

NOINLINE int recent_load(const char *fpath, t_rentry *rentries) {
  // Lines too long to be a path ("nor:", its path and "\r\n") are skipped.
  char buf[MAX_FN_LEN + 8];
  t_recent_read rd = { rentries, 0, false };
  FRESULT res = read_lines_file(fpath, buf, sizeof(buf), recent_line, &rd);
  WRITE_LOG("Loaded recently played games. %d entries found", rd.cnt);
  return rd.error ? -1 : FR_OK == res ? (int)rd.cnt : fr_missing(res) ? 0 : -1;
}


