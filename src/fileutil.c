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

bool read_lines(FIL *fd, char *buf, unsigned bufsize, line_fn cb, void *usr) {
  unsigned cnt = 0, pos = 0;  // Bytes in buf, where the next line starts
  bool eof = false, skipping = false;
  while (true) {
    char *line = &buf[pos], *nl = memchr(line, '\n', cnt - pos);
    if (!nl && !eof) {
      // Refill the buffer (the line so far moves to its start): a short read
      // is the end of the file. Full without a newline: too long, skipped
      // up to its newline.
      memmove(buf, line, cnt - pos);
      cnt -= pos;
      pos = 0;
      if (cnt == bufsize - 1) {
        if (!skipping && !cb(NULL, 0, usr))
          return true;
        skipping = true;
        cnt = 0;
      }
      UINT rdbytes;
      if (FR_OK != f_read(fd, &buf[cnt], bufsize - 1 - cnt, &rdbytes))
        return false;
      eof = cnt + rdbytes < bufsize - 1;
      cnt += rdbytes;
      continue;
    }
    if (pos == cnt)
      return true;

    unsigned len = nl ? (unsigned)(nl - line) : cnt - pos;
    pos += len + (nl ? 1 : 0);
    if (skipping) {
      skipping = false;       // Its end
      continue;
    }
    line[len] = 0;
    if (len && line[len - 1] == '\r')
      line[--len] = 0;        // Edited on Windows
    if (!cb(line, len, usr))
      return true;
  }
}

FRESULT read_lines_file(const char *fn, char *buf, unsigned bufsize, line_fn cb, void *usr) {
  FIL fd;
  FRESULT res = f_open(&fd, fn, FA_READ);
  if (res == FR_OK) {
    if (!read_lines(&fd, buf, bufsize, cb, usr))
      res = FR_DISK_ERR;
    f_close(&fd);
  }
  return res;
}

bool superfw_file_open(FIL *fd, const char *subdir, const char *fn, BYTE mode) {
  if (FR_OK == f_mkdir(SUPERFW_DIR))
    f_chmod(SUPERFW_DIR, AM_HID, AM_HID);     // Made now: hidden
  if (subdir)
    f_mkdir(subdir);
  return FR_OK == f_open(fd, fn, FA_WRITE | mode);
}

bool superfw_file_write(const char *subdir, const char *fn, const void *buf, unsigned len) {
  FIL fd;
  return superfw_file_open(&fd, subdir, WRITE_TMP_FILEPATH, FA_CREATE_ALWAYS) &&
         file_replace(WRITE_TMP_FILEPATH, fn, write_close(&fd, buf, len));
}

bool file_replace(const char *tmpfn, const char *fn, bool ok) {
  if (!ok) {
    f_unlink(tmpfn);
    return false;
  }
  // FAT can't rename over a file: fn is missing for a moment. If the rename
  // fails tmpfn is kept (fn may be gone: it's the only copy).
  f_unlink(fn);
  return FR_OK == f_rename(tmpfn, fn);
}

bool read_all(FIL *fd, void *buf, unsigned len) {
  UINT rdbytes;
  return FR_OK == f_read(fd, buf, len, &rdbytes) && rdbytes == len;
}

bool write_all(FIL *fd, const void *buf, unsigned len) {
  UINT wrbytes;
  return FR_OK == f_write(fd, buf, len, &wrbytes) && wrbytes == len;
}

bool write_close(FIL *fd, const void *buf, unsigned len) {
  const bool ok = write_all(fd, buf, len);
  return FR_OK == f_close(fd) && ok;
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

