/*
 * Copyright (C) 2026 Vinicius Fonseca <vinivius@gmail.com>
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.   See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef _FILEUTIL_H_
#define _FILEUTIL_H_

#include <stdbool.h>
#include "fatfs/ff.h"

// Whether a FatFs result means the file (or its folder) isn't there, as
// opposed to an error (ie. the SD card's) that leaves it unknown.
static inline bool fr_missing(FRESULT res) {
  return res == FR_NO_FILE || res == FR_NO_PATH;
}

// Reads a text file line by line, into buf: calls cb with each line (without
// its newline, nor a "\r" before it, NUL ended, anywhere in buf: not word
// aligned) until it returns false. A line that
// doesn't fit buf (bufsize - 2 chars) is skipped: cb gets NULL for it. False
// on read errors.
typedef bool (*line_fn)(char *line, unsigned len, void *usr);
bool read_lines(FIL *fd, char *buf, unsigned bufsize, line_fn cb, void *usr);
// Opens fn and reads it so. FR_OK, the f_open() result, or FR_DISK_ERR.
FRESULT read_lines_file(const char *fn, char *buf, unsigned bufsize, line_fn cb, void *usr);

// Writes len bytes to the open file fd and closes it (the data reaches the
// card then). False if they weren't all written.
bool write_close(FIL *fd, const void *buf, unsigned len);

#endif
