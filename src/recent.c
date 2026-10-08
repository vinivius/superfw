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

  // Flush to disk!
  FIL fo;
  if (FR_OK != f_open(&fo, RECENT_FILEPATH, FA_WRITE | FA_CREATE_ALWAYS))
    return false;

  // Write stuff to disk. Use a 1KiB buffer and flush as full blocks fill.
  unsigned coff = 0;
  char tmpbuf[1024];
  tmpbuf[0] = 0;

  for (unsigned i = 0; i < rcount; i++) {
    unsigned fnlen = strlen(rentries[i].fpath);
    if (rentries[i].flags & FLAG_RECENT_NOR) {
      memcpy(&tmpbuf[coff], "nor:", 4);
      coff += 4;
    }
    memcpy(&tmpbuf[coff], rentries[i].fpath, fnlen);
    coff += fnlen;
    tmpbuf[coff++] = '\n';

    if (coff >= 512) {
      UINT wrbytes;
      if (FR_OK != f_write(&fo, tmpbuf, 512, &wrbytes) || wrbytes != 512) {
        f_close(&fo);
        return false;
      }
      // Consume the first 512 written bytes
      memmove(&tmpbuf[0], &tmpbuf[512], coff - 512);
      coff -= 512;
    }
  }

  // Flush the last bytes (if any!)
  if (coff) {
    UINT wrbytes;
    if (FR_OK != f_write(&fo, tmpbuf, coff, &wrbytes) || wrbytes != coff) {
      f_close(&fo);
      return false;
    }
  }

  // The data reaches the card when it's closed.
  return FR_OK == f_close(&fo);
}

NOINLINE unsigned insert_recent_fn(t_rentry *rentries, unsigned rcount, const char *fn, unsigned flags) {
  WRITE_LOG("Adding/bumping recently played game: '%s' [%x]", fn, flags);

  for (unsigned i = 0; i < rcount; i++) {
    if (rentries[i].flags == flags && !strcmp(rentries[i].fpath, fn)) {
      // Found a matching file, move it to position 0, unless it's there already.
      if (i) {
        t_rentry tmp;
        memcpy32(&tmp, &rentries[i], sizeof(tmp));   // Copy entry to tmp
        memmove32(&rentries[1], &rentries[0], i * sizeof(rentries[0]));
        memcpy32(&rentries[0], &tmp, sizeof(tmp));
      }
      return rcount;
    }
  }

  // Not in the list, push all items back and insert it in the first position
  if (rcount) {
    unsigned movecnt = MIN(rcount, RECENT_MAXFN_CNT - 1);
    memmove32(&rentries[1], &rentries[0], movecnt * sizeof(rentries[0]));
  }

  const char *pbn = file_basename(fn);
  rentries[0].fname_offset = pbn - fn;
  rentries[0].flags = flags;
  memcpy32(rentries[0].fpath, fn, strlen(fn) + 1);
  // The oldest entry falls off the end when the list is full.
  return MIN(rcount + 1, RECENT_MAXFN_CNT);
}

NOINLINE unsigned delete_recent(t_rentry *rentries, unsigned rcount, unsigned entry_num) {
  if (entry_num >= rcount)
    return rcount;

  if (entry_num + 1 < rcount)
    memmove32(&rentries[entry_num], &rentries[entry_num + 1],
              (rcount - (entry_num + 1)) * sizeof(rentries[0]));

  return rcount - 1;
}

typedef struct {
  t_rentry *rentries;
  unsigned cnt;
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
    t_rentry *e = &rd->rentries[rd->cnt++];
    e->flags = nor ? FLAG_RECENT_NOR : 0;
    // Half word writes (SDRAM), from a line at any address (its NUL ends it).
    volatile uint16_t *d = (uint16_t*)e->fpath;
    unsigned i;
    for (i = 0; i < plen; i += 2)
      d[i / 2] = path[i] | (path[i + 1] << 8);
    if (i == plen)
      d[i / 2] = 0;
    e->fname_offset = file_basename(e->fpath) - e->fpath;
  }
  return rd->cnt < RECENT_MAXFN_CNT;
}

NOINLINE int recent_load(const char *fpath, t_rentry *rentries) {
  // Lines too long to be a path ("nor:", its path and "\r\n") are skipped.
  char buf[MAX_FN_LEN + 8];
  t_recent_read rd = { rentries, 0 };
  FRESULT res = read_lines_file(fpath, buf, sizeof(buf), recent_line, &rd);
  WRITE_LOG("Loaded recently played games. %d entries found", rd.cnt);
  return FR_OK == res ? (int)rd.cnt : fr_missing(res) ? 0 : -1;
}


