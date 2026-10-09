/*
 * Copyright (C) 2025 David Guillen Fandos <david@davidgf.net>
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

// File transfer over the link port UART (UART debug builds only).
//
// Entered from the menu loop after receiving 'X'. Commands are text lines,
// replies are text lines too ("OK ...", "ERR ..."):
//   LS <dir>            Lists a directory: "D <name>" / "F <size> <name>", then "END"
//   GET <file>          "OK <size>", then the file in blocks (see below)
//   PUT <size> <file>   "OK", then receives the file in blocks, then "DONE"
//   RM <file>           Deletes a file
//   QUIT                Leaves transfer mode ("BYE")
// Blocks are 4KiB (the last one is shorter): 'B' + data + checksum (8 bytes,
// checksum_words over the zero padded data). The receiver answers with 'A'
// (ok), 'N' (resend) or 'F' (fatal error, abort). The GBA UART only has a
// 4 byte receive FIFO, so nothing else (SD writes, logging) can happen while
// a block is being received; acknowledging every block keeps things in sync.

#ifdef ENABLE_UART_LOGGING

#pragma GCC optimize("Os")
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gbahw.h"
#include "common.h"
#include "util.h"
#include "fatfs/ff.h"
#include "fileutil.h"
#include "nanoprintf.h"

#define XFER_BLK         4096
#define FRAMES_PER_SEC     60

extern volatile unsigned frame_count;
void uart_write(const void *data, unsigned size);

volatile bool uart_xfer_active = false;    // Mutes UART logging, IRQ polling
static uint32_t xbuf[XFER_BLK / 4 + 1] __attribute__((section(".sbss")));   // EWRAM: IWRAM is tight

// Receives one byte, or -1 after the timeout (in frames).
static int ugetc(unsigned frames) {
  unsigned start = frame_count;
  while (REG_SIOCNT & (1 << 5)) {          // Receive FIFO empty
    if (frame_count - start > frames)
      return -1;
  }
  return REG_SIODATA8 & 0xFF;
}

static void uputs(const char *s) {
  uart_write(s, strlen(s));
}

// Discards anything pending until the line has been idle for a few frames.
static void udrain() {
  while (ugetc(3) >= 0);
}

static bool uread(void *dst, unsigned n, unsigned frames) {
  uint8_t *p = (uint8_t*)dst;
  while (n--) {
    int c = ugetc(frames);
    if (c < 0)
      return false;
    *p++ = c;
  }
  return true;
}

// Reads a command line. Returns false if the line stays idle for too long.
static bool ureadline(char *line, unsigned maxlen) {
  unsigned n = 0;
  while (1) {
    int c = ugetc(120 * FRAMES_PER_SEC);
    if (c < 0)
      return false;
    if (c == '\r')
      continue;
    if (c == '\n')
      break;
    if (n < maxlen - 1)
      line[n++] = c;
  }
  line[n] = 0;
  return true;
}

static void block_checksum(unsigned len, uint32_t *ck) {
  // Zero pad to a whole word, both sides checksum the padded data.
  uint8_t *p = (uint8_t*)xbuf;
  for (unsigned i = len; i & 3; i++)
    p[i] = 0;
  ck[0] = ck[1] = 0;
  checksum_words(xbuf, (len + 3) / 4, ck);
}

static void cmd_ls(const char *path) {
  DIR d;
  if (FR_OK != f_opendir(&d, path[0] ? path : "/")) {
    uputs("ERR cannot open dir\n");
    return;
  }
  uputs("OK\n");
  while (1) {
    FILINFO info;
    if (FR_OK != f_readdir(&d, &info) || !info.fname[0])
      break;
    char line[300];
    if (info.fattrib & AM_DIR)
      npf_snprintf(line, sizeof(line), "D %s\n", info.fname);
    else
      npf_snprintf(line, sizeof(line), "F %lu %s\n", (unsigned long)info.fsize, info.fname);
    uputs(line);
  }
  f_closedir(&d);
  uputs("END\n");
}

static void cmd_get(const char *path) {
  FIL fd;
  if (FR_OK != f_open(&fd, path, FA_READ)) {
    uputs("ERR cannot open file\n");
    return;
  }
  char line[32];
  npf_snprintf(line, sizeof(line), "OK %lu\n", (unsigned long)f_size(&fd));
  uputs(line);

  uint32_t remaining = f_size(&fd);
  while (remaining) {
    unsigned len = MIN(XFER_BLK, remaining);
    if (!read_all(&fd, xbuf, len)) {
      uart_write("F", 1);
      break;
    }
    uint32_t ck[2];
    block_checksum(len, ck);

    // Send the block until it is acknowledged.
    int ack;
    unsigned tries = 0;
    do {
      uart_write("B", 1);
      uart_write(xbuf, len);
      uart_write(ck, sizeof(ck));
      ack = ugetc(10 * FRAMES_PER_SEC);
    } while (ack == 'N' && ++tries < 8);
    if (ack != 'A')
      break;      // Aborted by the host (or lost)
    remaining -= len;
  }
  f_close(&fd);
}

static void cmd_put(char *args) {
  // Format: "<size> <path>"
  char *path = strchr(args, ' ');
  if (!path || !path[1]) {
    uputs("ERR bad arguments\n");
    return;
  }
  *path++ = 0;              // parseuint() parses the whole string
  uint32_t size = parseuint(args);

  // Write to a temporary file, replace the target once complete.
  char tmpfn[MAX_FN_LEN];
  if (npf_snprintf(tmpfn, sizeof(tmpfn), "%s.part", path) >= (int)sizeof(tmpfn)) {
    uputs("ERR path too long\n");
    return;
  }
  create_basepath(path);
  FIL fd;
  if (FR_OK != f_open(&fd, tmpfn, FA_WRITE | FA_CREATE_ALWAYS)) {
    uputs("ERR cannot create file\n");
    return;
  }
  uputs("OK\n");

  bool ok = true;
  uint32_t remaining = size;
  while (remaining && ok) {
    unsigned len = MIN(XFER_BLK, remaining);
    unsigned tries = 0;
    while (1) {
      uint32_t ck[2], rck[2];
      bool got = ugetc(10 * FRAMES_PER_SEC) == 'B' &&
                 uread(xbuf, len, FRAMES_PER_SEC) &&
                 uread(rck, sizeof(rck), FRAMES_PER_SEC);
      if (got) {
        block_checksum(len, ck);
        got = ck[0] == rck[0] && ck[1] == rck[1];
      }
      if (got)
        break;
      udrain();
      if (++tries >= 8) {
        uart_write("F", 1);
        ok = false;
        break;
      }
      uart_write("N", 1);
    }
    if (!ok)
      break;

    if (!write_all(&fd, xbuf, len)) {
      uart_write("F", 1);
      ok = false;
      break;
    }
    uart_write("A", 1);
    remaining -= len;
  }

  ok = FR_OK == f_close(&fd) && ok;
  uputs(file_replace(tmpfn, path, ok) ? "DONE\n" : "ERR transfer failed\n");
}

void uart_xfer_mode() {
  uart_xfer_active = true;
  udrain();
  uputs("\nXFER READY\n");

  while (1) {
    char line[MAX_FN_LEN + 32];
    if (!ureadline(line, sizeof(line)))
      break;          // Idle for too long, go back to the menu

    if (!strncmp(line, "LS", 2))
      cmd_ls(line[2] == ' ' ? &line[3] : "");
    else if (!strncmp(line, "GET ", 4))
      cmd_get(&line[4]);
    else if (!strncmp(line, "PUT ", 4))
      cmd_put(&line[4]);
    else if (!strncmp(line, "RM ", 3))
      uputs(FR_OK == f_unlink(&line[3]) ? "OK\n" : "ERR cannot delete\n");
    else if (!strcmp(line, "QUIT"))
      break;
    else if (line[0])
      uputs("ERR unknown command\n");
  }

  uputs("BYE\n");
  uart_xfer_active = false;
}

#endif
