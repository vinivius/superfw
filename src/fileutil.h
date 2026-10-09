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

// Creates SUPERFW_DIR (hidden) and subdir if any (in it), if missing, and
// opens fn in them to write it (FA_WRITE | mode).
bool superfw_file_open(FIL *fd, const char *subdir, const char *fn, BYTE mode);

// The temporary file whole-file writes go through (FatFs moves it across
// folders: its name never gets too long).
#define WRITE_TMP_FILEPATH   SUPERFW_DIR "/write.tmp"

// Writes fn whole (making SUPERFW_DIR, and subdir if any, if missing) through
// WRITE_TMP_FILEPATH, which replaces it once written: a failure keeps the
// old one.
bool superfw_file_write(const char *subdir, const char *fn, const void *buf, unsigned len);

// Finishes writing fn through tmpfn (closed): if ok it replaces fn, else it's
// removed. False if fn wasn't replaced (tmpfn is kept if only the rename
// failed).
bool file_replace(const char *tmpfn, const char *fn, bool ok);

// Reads len bytes from the open file fd. False if they weren't all read.
bool read_all(FIL *fd, void *buf, unsigned len);

// Writes len bytes to the open file fd. False if they weren't all written.
bool write_all(FIL *fd, const void *buf, unsigned len);

// Writes len bytes to the open file fd and closes it (the data reaches the
// card then). False if they weren't all written.
bool write_close(FIL *fd, const void *buf, unsigned len);

#endif
