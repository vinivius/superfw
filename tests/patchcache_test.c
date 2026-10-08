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

// Patch files (the patch engine cache): the format version they are written
// with, and which older files are still trusted.

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "patchengine.h"

#define OP_EEPROM_HD(addr)  ((0x8u << 28) | (addr))
#define OP_FLASH_HD(addr)   ((0x9u << 28) | (addr))

static t_patch p, q;
static uint8_t buf[1024];

static int write_patch(uint32_t save_op, const char *magic) {
  memset(&p, 0, sizeof(p));
  p.wcnt_ops = 2;                       // Save ops come after the WAITCNT ones
  p.op[0] = p.op[1] = 0x1000;
  p.save_ops = 1;
  p.op[2] = save_op;
  int size = serialize_patch(&p, buf);
  if (magic)
    memcpy(buf, magic, 16);
  return size;
}

int main() {
  // Written as V02 and read back.
  int size = write_patch(OP_FLASH_HD(0x1234), NULL);
  assert(size <= (int)sizeof(buf));
  assert(!memcmp(buf, "SUPERFWPATCHV02", 16));
  assert(unserialize_patch(buf, size, &q));
  assert(q.wcnt_ops == 2 && q.save_ops == 1 && q.op[2] == OP_FLASH_HD(0x1234));

  // V01 files predate the v1 flash handler fix: refused with flash handlers,
  // still used without them.
  assert(!unserialize_patch(buf, write_patch(OP_FLASH_HD(0x1234), "SUPERFWPATCHV01"), &q));
  assert(unserialize_patch(buf, write_patch(OP_EEPROM_HD(0x1234), "SUPERFWPATCHV01"), &q));
  assert(q.op[2] == OP_EEPROM_HD(0x1234));

  // Op counts that don't fit the op table are refused (corrupted files).
  size = write_patch(OP_EEPROM_HD(0x1234), NULL);
  buf[16] = 120;                        // wcnt_ops
  buf[17] = 9;                          // save_ops: 129 ops in all
  assert(!unserialize_patch(buf, size, &q));
  buf[17] = 8;                          // 128: the whole table
  assert(unserialize_patch(buf, size, &q));

  // Unknown versions and wrong sizes are refused.
  assert(!unserialize_patch(buf, write_patch(OP_EEPROM_HD(0x1234), "SUPERFWPATCHV03"), &q));
  assert(!unserialize_patch(buf, size - 1, &q));

  puts("Patch file tests OK");
  return 0;
}
