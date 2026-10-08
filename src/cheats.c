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

#include "cheats.h"
#include "util.h"
#include "fatfs/ff.h"
#include "fileutil.h"

#pragma GCC optimize ("Os")

// Preprocessing cheats, includes a proper header and pre-formats some payloads.
// The values codes 4 (an iteration count and increments) and 5 (halfwords to
// write, 3 a line) take follow them, as more codes. False if one takes more
// than there are, or has a count of 0 (cheat_exec would loop 2^32 times).
bool predecode_cheats(uint32_t *codes, unsigned cnt) {
  for (unsigned i = 0; i < cnt; i++) {
    const unsigned opcode = (codes[2*i] >> 28) & 0xF;
    const uint16_t value = codes[2*i+1];
    const unsigned extra = (opcode == 4) ? 1 : (opcode == 5) ? (value + 2) / 3 : 0;
    if (i + extra >= cnt || (opcode == 5 && !value) || (opcode == 4 && !(uint16_t)codes[2*i+2]))
      return false;

    // Overwrite buffer with the new format
    t_cheat_predec h = {
      .opcode = opcode * 2,             // Opcode scaled by 2
      .blen = (extra + 1) * 8,          // Its size (conditional codes skip it)
      .value = value,
      .address = codes[2*i] & 0xFFFFFFF,
    };
    memcpy(&codes[2*i], &h, sizeof(h));

    // Code 5's data: endianess conversion.
    for (unsigned j = 0; j < extra && opcode == 5; j++) {
      i++;
      uint32_t addr = codes[2*i];
      uint16_t valu = codes[2*i+1];
      codes[2*i] = __builtin_bswap32(addr);
      codes[2*i+1] = __builtin_bswap16(valu);
    }
    if (opcode == 4)
      i++;
  }
  return true;
}

static bool parse_hex(const char *s, uint32_t *val, unsigned nibcnt) {
  unsigned r = 0;
  for (unsigned i = 0; i < nibcnt; i++) {
    if (!s[i])
      return false;    // String ended prematurely

    if (s[i] >= '0' && s[i] <= '9')
      r = (r << 4) | (s[i] - '0');
    else if (s[i] >= 'a' && s[i] <= 'f')
      r = (r << 4) | (s[i] - 'a' + 10);
    else if (s[i] >= 'A' && s[i] <= 'F')
      r = (r << 4) | (s[i] - 'A' + 10);
    else
      return false;
  }
  *val = r;
  return true;
}

// Parses RAW codes into a uint32 buffer (two words a code), up to
// MAX_CHEAT_CODES of them (-1 if there are more).
int parse_cheat_codes(const char *s, uint32_t *codes) {
  // Codes are in the format:
  // 0123ABCD+67EF 125634AB+78CD ....
  // We parse them assuming that the separators are space or plus. We admit multiple separators.
  unsigned cnt = 0;

  while (*s == ' ' || *s == '+') s++;      // Skip initial spaces

  while (*s) {
    uint32_t addr, val;
    if (cnt == MAX_CHEAT_CODES || !parse_hex(s, &addr, 8))
      return -1;

    s += 8;  // Consume the hex32
    while (*s == ' ' || *s == '+') s++;      // Skip separators
    if (!*s)
      return -1;   // The code is truncated, abort.

    if (!parse_hex(s, &val, 4))
      return -1;
    s += 4;  // Consume the hex16

    // A full code (addr+val) has been parsed, just write it raw into the buffer.
    *codes++ = addr;
    *codes++ = val;
    cnt++;

    while (*s == ' ' || *s == '+') s++;      // Skip trailing separators
  }
  return cnt;
}


typedef struct {
  uint8_t *buffer;
  unsigned size, used, count;
  bool codes;               // The next line is a cheat's codes (else its title)
  bool error;               // Out of space, or a write that never verified
  t_cheathdr_ext chdr;
} t_cheat_read;

// A line of the cheat file: titles and code lines alternate (empty lines are
// skipped). A cheat whose codes can't be used is left out.
static bool cheat_line(char *line, unsigned len, void *usr) {
  t_cheat_read *cr = (t_cheat_read*)usr;
  while (*line == ' ' || *line == '\t')
    line++;
  if (!*line)
    return true;

  if (cr->codes) {
    // The codes, in hex: generate the predecoded ones.
    cr->codes = false;
    uint32_t codes[2 * (MAX_CHEAT_CODES + 1)];  // And the end one
    memset(codes, 0, sizeof(codes));
    int numcodes = parse_cheat_codes(line, codes);
    if (numcodes <= 0 || !predecode_cheats(codes, numcodes))
      return true;
    cr->chdr.h.codelen = 8 * (numcodes + 1);

    // The cheats go to the cart's SDRAM: writes are checked.
    unsigned pheadl = sizeof(t_cheathdr) + cr->chdr.h.slen;
    cr->error = cr->used + pheadl + cr->chdr.h.codelen > cr->size ||
                !memcpy32_checked(&cr->buffer[cr->used], &cr->chdr, pheadl) ||
                !memcpy32_checked(&cr->buffer[cr->used + pheadl], codes, cr->chdr.h.codelen);
    cr->used += pheadl + cr->chdr.h.codelen;
    cr->count++;
    return !cr->error;
  }

  // The title: long ones are cut (at a character start).
  len = strlen(line);
  if (len > MAX_CHEAT_TITLE)
    len = MAX_CHEAT_TITLE;
  while (len && (line[len] & 0xC0) == 0x80)
    len--;
  memcpy(cr->chdr.title, line, len);
  cr->chdr.title[len] = 0;
  cr->chdr.h.slen = (len + 1 + 3) & ~3U;  // Word aligned!
  cr->chdr.h.enabled = 0;
  cr->codes = true;
  return true;
}

// Reads a cheat file into a buffer (usually in SDRAM): the count of cheats,
// then each one. Returns its size in bytes, or -1 if the file can't be read
// or has no cheat that can be used.
int open_read_cheats(uint8_t *buffer, unsigned buffsize, const char *fn) {
  FIL fd;
  if (FR_OK != f_open(&fd, fn, FA_READ))
    return -1;
  // Lines too long for any cheat are skipped.
  char tmp[1024];
  t_cheat_read cr = { .buffer = buffer, .size = buffsize, .used = 4, .codes = false };
  bool ok = read_lines(&fd, tmp, sizeof(tmp), cheat_line, &cr);
  f_close(&fd);
  return ok && !cr.error && cr.count && memcpy32_checked(buffer, &cr.count, 4) ? (int)cr.used : -1;
}
