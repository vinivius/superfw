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

// Patching ROMs: patches applied to the ROM in chunks (as NOR flashing does)
// give the same ROM as applied to all of it, also where the code they write
// spans chunks. Random (valid) patches.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "patchengine.h"

// The handlers patches copy (GBA code): stand-ins of known content and sizes.
#define PAYLOADS(X)                                                            \
  X(patch_rtc_probe, 7) X(patch_rtc_getstatus, 9) X(patch_rtc_gettimedate, 11) \
  X(patch_rtc_reset, 5) X(patch_eeprom_read_sram64k, 13)                       \
  X(patch_eeprom_write_sram64k, 17) X(patch_eeprom_read_directsave, 6)         \
  X(patch_eeprom_write_directsave, 8) X(patch_flash_read_sram64k, 10)          \
  X(patch_flash_write_sector_sram64k, 12) X(patch_flash_write_byte_sram64k, 14)\
  X(patch_flash_erase_sector_sram64k, 16) X(patch_flash_erase_device_sram64k, 18) \
  X(patch_flash_read_sram128k, 20) X(patch_flash_write_sector_sram128k, 22)    \
  X(patch_flash_write_byte_sram128k, 24) X(patch_flash_erase_sector_sram128k, 26) \
  X(patch_flash_erase_device_sram128k, 28) X(patch_flash_read_directsave, 30)  \
  X(patch_flash_write_sector_directsave, 32) X(patch_flash_write_byte_directsave, 34) \
  X(patch_flash_erase_sector_directsave, 36) X(patch_flash_erase_device_directsave, 38)

#define DEFINE_PAYLOAD(name, n) uint16_t name[n]; const uint32_t name##_size = (n) * 2;
PAYLOADS(DEFINE_PAYLOAD)

#define ROM_SIZE   (64*1024)

static uint32_t rnd_state = 1;
static uint32_t rnd() {
  rnd_state ^= rnd_state << 13;
  rnd_state ^= rnd_state >> 17;
  rnd_state ^= rnd_state << 5;
  return rnd_state;
}

// Random ops (with the data words copy ops take), at most max words.
static unsigned gen_ops(uint32_t *ops, unsigned max) {
  static const unsigned opcs[] = { 0, 1, 2, 3, 4, 5, 7, 8, 9 };
  static const unsigned fnargs[] = { 0, 1, 4, 5 };
  unsigned n = 0;
  while (true) {
    unsigned opc = opcs[rnd() % 9], arg = rnd() % 8, data = 0;
    uint32_t moff = rnd() % (ROM_SIZE - 128);
    switch (opc) {
      case 0: arg %= MAX_PATCH_PRG; break;
      case 3: data = (arg + 4) / 4; break;
      case 4: data = arg + 1; break;
      case 5: arg = fnargs[arg % 4]; break;
      case 7: arg %= 4; break;
      case 8: arg %= 2; break;
      case 9: arg %= 5; break;
    }
    if (opc != 0 && opc != 2 && opc != 3 && opc != 4)
      moff &= ~1U;            // Code (thumb, handlers) is half word aligned
    if (n + 1 + data > max)
      return n;
    ops[n++] = (opc << 28) | (arg << 25) | moff;
    for (unsigned i = 0; i < data; i++)
      ops[n++] = rnd();
  }
}

static void gen_patch(t_patch *p) {
  memset(p, 0, sizeof(*p));
  p->save_mode = 1 + rnd() % 5;
  for (unsigned i = 0; i < MAX_PATCH_PRG; i++) {
    p->prgs[i].length = 1 + rnd() % sizeof(p->prgs[i].data);
    for (unsigned j = 0; j < p->prgs[i].length; j++)
      p->prgs[i].data[j] = rnd();
  }
  unsigned n = 0;
  p->wcnt_ops = gen_ops(&p->op[n], rnd() % 32);
  n += p->wcnt_ops;
  p->save_ops = gen_ops(&p->op[n], rnd() % 32);
  n += p->save_ops;
  p->irqh_ops = gen_ops(&p->op[n], rnd() % 32);
  n += p->irqh_ops;
  p->rtc_ops = gen_ops(&p->op[n], rnd() % 16);
  assert(patch_check(p, NULL));
}

static uint8_t rom[ROM_SIZE], whole[ROM_SIZE], chunked[ROM_SIZE], tmp[ROM_SIZE];
static uint16_t payload[512];

static void apply(uint8_t *buf, unsigned size, uint32_t base, const t_patch *p,
                  uint32_t igm, uint32_t ds, uint32_t poff, unsigned psize) {
  patch_apply_rom(buf, size, base, true, p, true, igm, ds);
  payload_apply_rom(buf, size, base, (uint8_t*)payload, psize, poff);
}

int main() {
  // Handler contents.
  uint16_t v = 1;
  #define FILL_PAYLOAD(name, n) for (unsigned i = 0; i < n; i++) name[i] = v++ * 0x9E37;
  PAYLOADS(FILL_PAYLOAD)
  for (unsigned i = 0; i < sizeof(payload) / 2; i++)
    payload[i] = rnd();

  // Copy ops write the data that follows them.
  {
    t_patch p = {0};
    p.save_ops = 4;
    p.op[0] = (0x2u << 28) | 0x40;                 // An ARM nop
    p.op[1] = (0x3u << 28) | (4u << 25) | 0x101;   // 5 bytes
    p.op[2] = 0x44332211;
    p.op[3] = 0x00000055;
    memset(whole, 0, sizeof(whole));
    patch_apply_rom(whole, ROM_SIZE, 0, false, &p, false, 0, 0);
    assert(!memcmp(&whole[0x101], "\x11\x22\x33\x44\x55", 5) && whole[0x106] == 0);
  }

  for (unsigned t = 0; t < 3000; t++) {
    t_patch p;
    gen_patch(&p);
    for (unsigned i = 0; i < ROM_SIZE; i++)
      rom[i] = rnd();
    const uint32_t igm = rnd() & 1 ? 0x08000000 + ((rnd() % ROM_SIZE) & ~3U) : 0;
    const uint32_t ds = rnd() & 1 ? 0x08000000 + ((rnd() % ROM_SIZE) & ~3U) : 0;
    const uint32_t poff = (rnd() % (ROM_SIZE - sizeof(payload))) & ~1U;
    const unsigned psize = 2 + rnd() % (sizeof(payload) - 2);

    memcpy(whole, rom, ROM_SIZE);
    apply(whole, ROM_SIZE, 0, &p, igm, ds, poff, psize);

    // Chunks of any (word multiple) size; the header patch (in-game menu
    // entry point) needs the first one to hold the header.
    for (unsigned off = 0, size; off < ROM_SIZE; off += size) {
      size = 4 * (1 + rnd() % 1024);
      if (!off && size < 0xC0)
        size = 0xC0;
      if (size > ROM_SIZE - off)
        size = ROM_SIZE - off;
      memcpy(tmp, &rom[off], size);
      apply(tmp, size, off, &p, igm, ds, poff, psize);
      memcpy(&chunked[off], tmp, size);
    }
    assert(!memcmp(whole, chunked, ROM_SIZE));
  }

  puts("Patcher tests OK");
  return 0;
}
