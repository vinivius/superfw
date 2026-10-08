/*
 * Copyright (C) 2024 David Guillen Fandos <david@davidgf.net>
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

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "fatfs/ff.h"
#include "fileutil.h"
#include "config.h"

#pragma GCC optimize ("Os")

bool check_file_exists(const char *fn) {
  return FR_OK == f_stat(fn, NULL);
}

bool read_lines(FIL *fd, char *buf, unsigned bufsize, line_fn cb, void *usr) {
  unsigned cnt = 0;           // Bytes in buf
  bool skipping = false;      // In a line too long for buf
  while (true) {
    // Fill the buffer: a short read is the end of the file.
    UINT rdbytes;
    if (FR_OK != f_read(fd, &buf[cnt], bufsize - 1 - cnt, &rdbytes))
      return false;
    cnt += rdbytes;
    if (!cnt)
      return true;

    char *nl = memchr(buf, '\n', cnt);
    unsigned len = nl ? (unsigned)(nl - buf) : cnt;
    if (!nl && cnt == bufsize - 1) {
      skipping = true;        // Too long, skipped up to its newline
      cnt = 0;
      continue;
    }
    if (skipping)
      skipping = false;       // Its end
    else {
      buf[len] = 0;
      if (len && buf[len - 1] == '\r')
        buf[--len] = 0;       // Edited on Windows
      if (!cb(buf, len, usr))
        return true;
    }

    // Consume the line
    unsigned used = nl ? (unsigned)(nl - buf) + 1 : cnt;
    memmove(buf, &buf[used], cnt - used);
    cnt -= used;
  }
}

// Creates the path for a given file name.
void create_basepath(const char *fn) {
  if (!fn || !*fn)
    return;        // Empty path

  char tmp[MAX_FN_LEN];
  strcpy(tmp, fn);

  // Iteratively attempt to create dirs, will fail if the dir already exists.
  unsigned off = 1;   // Skip first char (if it's a "/" we don't care)
  while (true) {
    char *p = strchr(&tmp[off], '/');
    if (!p)
      return;          // All done!

    *p = 0;            // Temporarily truncate the char
    f_mkdir(tmp);      // Create the dir until here
    *p = '/';

    off = p - tmp + 1; // Advance pointer
  }
}

