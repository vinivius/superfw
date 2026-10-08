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

// Patch files (the patch engine cache): the format they are written in, and
// which files from older firmwares are still trusted.

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "patchengine.h"

#define OP_EEPROM_HD(addr)  ((0x8u << 28) | (addr))
#define OP_FLASH_HD(h, addr) ((0x9u << 28) | ((h) << 25) | (addr))
#define FLASH_CLRC 1        // Handlers: chip erase, sector erase,
#define FLASH_CLRS 2        // sector write, byte write
#define FLASH_WRTS 3
#define FLASH_WRBT 4

static t_patch p, q;
static uint8_t buf[1024];

// A patch with the given save ops (after 2 WAITCNT ones), with magic if set.
static int write_patch_ops(const uint32_t *ops, unsigned n, const char *magic) {
  memset(&p, 0, sizeof(p));
  p.wcnt_ops = 2;
  p.op[0] = p.op[1] = 0x1000;
  p.save_ops = n;
  memcpy(&p.op[2], ops, n * sizeof(ops[0]));
  int size = serialize_patch(&p, buf);
  if (magic)
    memcpy(buf, magic, 16);
  return size;
}

static int write_patch(uint32_t save_op, const char *magic) {
  return write_patch_ops(&save_op, 1, magic);
}

int main() {
  // Written as V02 and read back.
  const uint32_t v1table[] = { OP_FLASH_HD(FLASH_CLRC, 0x100), OP_FLASH_HD(FLASH_CLRS, 0x200),
                               OP_FLASH_HD(FLASH_WRTS, 0x300) };
  const uint32_t v2table[] = { OP_FLASH_HD(FLASH_CLRC, 0x100), OP_FLASH_HD(FLASH_CLRS, 0x200),
                               OP_FLASH_HD(FLASH_WRTS, 0x300), OP_FLASH_HD(FLASH_WRBT, 0x400) };
  int size = write_patch_ops(v1table, 3, NULL);
  assert(size <= (int)sizeof(buf));
  assert(!memcmp(buf, "SUPERFWPATCHV01", 16) && buf[21] == 1);    // Same format as upstream, flagged
  assert(unserialize_patch(buf, size, &q));
  assert(q.wcnt_ops == 2 && q.save_ops == 3 && q.op[3] == OP_FLASH_HD(FLASH_CLRS, 0x200));

  // Files without the flag (older firmwares write 0) predate the fix for v1
  // flash tables: refused if made from one (erase and write handlers, no byte
  // write one), used otherwise.
  write_patch_ops(v1table, 3, NULL);
  buf[21] = 0;
  assert(!unserialize_patch(buf, size, &q));
  write_patch_ops(v2table, 4, NULL);
  buf[21] = 0;
  assert(unserialize_patch(buf, size, &q));
  write_patch(OP_FLASH_HD(5, 0x1234), NULL);
  buf[21] = 0;
  assert(unserialize_patch(buf, size, &q));
  write_patch(OP_EEPROM_HD(0x1234), NULL);
  buf[21] = 0;
  assert(unserialize_patch(buf, size, &q));
  assert(q.op[2] == OP_EEPROM_HD(0x1234));

  // Op counts that don't fit the op table are refused (corrupted files).
  size = write_patch(OP_EEPROM_HD(0x1234), NULL);
  buf[16] = 120;                        // wcnt_ops
  buf[17] = 9;                          // save_ops: 129 ops in all
  assert(!unserialize_patch(buf, size, &q));
  buf[17] = 8;                          // 128: the whole table
  assert(unserialize_patch(buf, size, &q));

  // Programs longer than their data, or ops writing a program that doesn't
  // exist, are refused.
  write_patch(OP_EEPROM_HD(0x1234), NULL);
  p.prgs[1].length = sizeof(p.prgs[1].data) + 1;
  serialize_patch(&p, buf);
  assert(!unserialize_patch(buf, size, &q));
  write_patch((0u << 28) | (MAX_PATCH_PRG << 25) | 0x1234, NULL);
  assert(!unserialize_patch(buf, size, &q));
  write_patch((0u << 28) | ((MAX_PATCH_PRG - 1) << 25) | 0x1234, NULL);
  assert(unserialize_patch(buf, size, &q));
  write_patch((0x7u << 28) | (4u << 25) | 0x1234, NULL);     // RTC handlers are 0-3
  assert(!unserialize_patch(buf, size, &q));
  write_patch((0x7u << 28) | (3u << 25) | 0x1234, NULL);
  assert(unserialize_patch(buf, size, &q));

  // Copy ops are followed by data, which isn't checked as ops (this word would
  // be an op writing program 5), but must fit in the op table.
  {
    const uint32_t copy[] = { (0x4u << 28) | (0u << 25) | 0x100, (0x0u << 28) | (5u << 25) | 0x1234 };
    write_patch_ops(copy, 2, NULL);
    assert(unserialize_patch(buf, size, &q));
    write_patch_ops(copy, 1, NULL);       // Its data word missing
    assert(!unserialize_patch(buf, size, &q));
  }

  // A hole (where payloads go) past the 32 MiB of ROM is refused.
  write_patch(OP_EEPROM_HD(0x1234), NULL);
  p.hole_addr = 32*1024*1024 - 1024;
  p.hole_size = 2048;
  serialize_patch(&p, buf);
  assert(!unserialize_patch(buf, size, &q));
  p.hole_size = 1024;
  serialize_patch(&p, buf);
  assert(unserialize_patch(buf, size, &q) && q.hole_addr == p.hole_addr && q.hole_size == 1024);

  // Unknown versions and wrong sizes are refused.
  assert(!unserialize_patch(buf, write_patch(OP_EEPROM_HD(0x1234), "SUPERFWPATCHV02"), &q));
  assert(!unserialize_patch(buf, size - 1, &q));

  puts("Patch file tests OK");
  return 0;
}
