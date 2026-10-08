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

#include "gbahw.h"
#include "fatfs/ff.h"
#include "common.h"
#include "patches.h"
#include "patchengine.h"
#include "util.h"

#pragma GCC optimize ("Os")

typedef struct {
  uint32_t signature;    // "PTDB" in ASCII
  uint32_t dbversion;    // Format version
  uint32_t patchcnt;     // Patch count
  uint32_t idxcnt;       // Number of IDX blocks
  char date[8];          // Creating date (ASCII encoded)
  char version[8];       // DB version (ASCII encoded)
  char creator[32];      // DB author/creator (ASCII encoded)
} t_db_header;

typedef struct {
  uint8_t gcode[4];
  uint32_t offset;       // LSB is game version (8 bits), MSB is byte offset
} t_db_idx;

// Game code is 4 bytes long (ascii chars in theory) + 1 byte for versioning
static int gcodecmp(const uint8_t *g1, const uint8_t *g2) {
  for (unsigned i = 0; i < 5; i++) {
    if (g1[i] < g2[i])
      return -1;
    if (g1[i] > g2[i])
      return 1;
  }
  return 0;
}

void patchmem_dbinfo(const uint8_t *dbptr, uint32_t *pcnt, char *version, char *date, char *creator) {
  const t_db_header *dbh = (t_db_header*)dbptr;
  *pcnt = dbh->patchcnt;
  memcpy(date, dbh->date, sizeof(dbh->date));
  memcpy(version, dbh->version, sizeof(dbh->version));
  memcpy(creator, dbh->creator, sizeof(dbh->creator));
}

// Routines to lookup patches from the patch database in memory
bool patchmem_lookup(const uint8_t *gamecode, const uint8_t *dbptr, t_patch *pdata) {
  const t_db_header *dbh = (t_db_header*)dbptr;
  if (dbh->signature != 0x31424450 ||      // PTDB signature mismatch
      dbh->dbversion != 0x00010000)        // Version check
    return false;

  // A database loaded from the SD card may be corrupted: what's read must be
  // in its space (the entries' contents are checked too).
  const unsigned dbspace = ROM_OFF_ASSETS_BASE - ROM_OFF_PATCH_DB;
  if (dbh->idxcnt > (dbspace - 1024) / 512 || dbh->patchcnt > dbh->idxcnt * (512 / sizeof(t_db_idx)))
    return false;
  const unsigned maxwords = (dbspace - 1024 - 512 * dbh->idxcnt) / 4;

  // Skip header and program block.
  const t_db_idx *dbidx = (t_db_idx*)&dbptr[1024];
  // Skip the index block to address data entries.
  const uint32_t *entries = (uint32_t*)&dbptr[1024 + 512 * dbh->idxcnt];

  // Load programs as well
  int pgn = 0;
  const uint8_t *pgrpage = &dbptr[512];
  for (int i = 0; i < MAX_PATCH_PRG; i++)
    pdata->prgs[i].length = 0;
  for (int i = 0; i < 512 && pgn < MAX_PATCH_PRG; i++) {
    unsigned cnt = pgrpage[i];
    if (!cnt)
      break;

    if (cnt > sizeof(pdata->prgs[pgn].data))
      return false;

    pdata->prgs[pgn].length = cnt;
    memcpy(pdata->prgs[pgn++].data, &pgrpage[i+1], cnt);
    i += cnt;
  }

  for (unsigned i = 0; i < dbh->patchcnt; i++) {
    if (!gcodecmp(dbidx[i].gcode, gamecode)) {
      uint32_t offset = dbidx[i].offset >> 8;
      if (offset >= maxwords)
        return false;
      const uint32_t *p = &entries[offset];
      const uint32_t pheader = *p++;

      pdata->wcnt_ops = (pheader >>  0) & 0xFF;
      pdata->save_ops = (pheader >>  8) & 0x1F;    // Only 5 bits
      pdata->irqh_ops = (pheader >> 16) & 0xFF;
      pdata->rtc_ops =  (pheader >> 24) & 0x0F;    // Only 4 bits

      pdata->save_mode = (pheader >> 13) & 0x7;    // 3 bits

      const unsigned numops = pdata->wcnt_ops + pdata->save_ops + pdata->irqh_ops + pdata->rtc_ops;
      if (numops > MAX_PATCH_OPS || offset + numops + 2 > maxwords)
        return false;         // A corrupted database (ie. loaded from the SD card)

      pdata->hole_addr = pdata->hole_size = 0;
      if ((pheader >> 28) & 0x1) {
        // Hole/Trailing space information, placed in the last op
        pdata->hole_addr = (p[numops] >> 16) << 10;   // In KiB chunks
        pdata->hole_size = (p[numops] & 0xFFFF) << 10;
      }

      // Copy patch words
      memcpy(&pdata->op[0], p, numops * sizeof(uint32_t));

      return patch_check(pdata, NULL);
    }
  }

  return false;
}

// The part of a ROM patches are applied to: ROM addresses [base, base + size),
// loaded at buf. Writes outside it are dropped (a chunked apply writes the
// rest with the other chunks).
typedef struct {
  uint8_t *buf;
  uint32_t base;
  unsigned size;
  bool failed;          // A write never read back as written
} t_rom_part;

// Writes are checked (write16_checked()).
static void put_half(t_rom_part *rp, volatile uint16_t *ptr, uint16_t data) {
  if (!write16_checked(ptr, data))
    rp->failed = true;
}

// Writes a byte at a ROM address, with 16 bit accesses only.
static void put8(t_rom_part *rp, uint32_t addr, uint8_t bytedata) {
  if (addr - rp->base < rp->size) {
    uintptr_t ptraddr = (uintptr_t)&rp->buf[addr - rp->base];
    volatile uint16_t *aptr = (uint16_t*)(ptraddr & ~(uintptr_t)1);
    unsigned sha = (ptraddr & 1) ? 8 : 0;
    put_half(rp, aptr, (*aptr & ~(0xFF << sha)) | (bytedata << sha));
  }
}

static void put16(t_rom_part *rp, uint32_t addr, uint16_t data) {
  put8(rp, addr + 0, data >> 0);
  put8(rp, addr + 1, data >> 8);
}

static void put32(t_rom_part *rp, uint32_t addr, uint32_t data) {
  put16(rp, addr + 0, data >>  0);
  put16(rp, addr + 2, data >> 16);
}

// Copies a function (or payload) to a (half word aligned) ROM address.
static void copy_func16(t_rom_part *rp, uint32_t addr, const void *fnptr, unsigned size) {
  const uint16_t *src = (uint16_t*)((uintptr_t)fnptr & ~(uintptr_t)1);   // Clear thumb addr bit for the symbol
  for (unsigned i = 0; i < size; i += 2)
    if (addr + i - rp->base < rp->size)
      put_half(rp, (uint16_t*)&rp->buf[addr + i - rp->base], src[i / 2]);
}

// Flashing/Eeprom routines flavours:
typedef struct {
  void *ptr;
  const uint32_t *size;
} f_func_info;

typedef struct {
  f_func_info eeprom_fncs[2];    // read and write
  f_func_info flash_fncs[5];     // read, erase-device, erase-sector, write-sector, write-byte
} t_psave_funcs;

typedef struct {
  uint32_t dspayload_addr;
  const t_psave_funcs *sfns;
} t_psave_info;

#define SFUNC(start, end) { (start), (end) - (start) },

static const t_psave_funcs psram_conversion_64k = {
  {
    { patch_eeprom_read_sram64k,        &patch_eeprom_read_sram64k_size },
    { patch_eeprom_write_sram64k,       &patch_eeprom_write_sram64k_size },
  },
  {
    { patch_flash_read_sram64k,         &patch_flash_read_sram64k_size },
    { patch_flash_erase_device_sram64k, &patch_flash_erase_device_sram64k_size },
    { patch_flash_erase_sector_sram64k, &patch_flash_erase_sector_sram64k_size },
    { patch_flash_write_sector_sram64k, &patch_flash_write_sector_sram64k_size },
    { patch_flash_write_byte_sram64k,   &patch_flash_write_byte_sram64k_size },
  },
};

static const t_psave_funcs psram_conversion_128k = {
  {
    { patch_eeprom_read_sram64k,        &patch_eeprom_read_sram64k_size },
    { patch_eeprom_write_sram64k,       &patch_eeprom_write_sram64k_size },
  },
  {
    { patch_flash_read_sram128k,         &patch_flash_read_sram128k_size },
    { patch_flash_erase_device_sram128k, &patch_flash_erase_device_sram128k_size },
    { patch_flash_erase_sector_sram128k, &patch_flash_erase_sector_sram128k_size },
    { patch_flash_write_sector_sram128k, &patch_flash_write_sector_sram128k_size },
    { patch_flash_write_byte_sram128k,   &patch_flash_write_byte_sram128k_size },
  },
};

static const t_psave_funcs pdirectsave = {
  {
    { patch_eeprom_read_directsave,        &patch_eeprom_read_directsave_size },
    { patch_eeprom_write_directsave,       &patch_eeprom_write_directsave_size },
  },
  {
    { patch_flash_read_directsave,         &patch_flash_read_directsave_size },
    { patch_flash_erase_device_directsave, &patch_flash_erase_device_directsave_size },
    { patch_flash_erase_sector_directsave, &patch_flash_erase_sector_directsave_size },
    { patch_flash_write_sector_directsave, &patch_flash_write_sector_directsave_size },
    { patch_flash_write_byte_directsave,   &patch_flash_write_byte_directsave_size },
  },
};

static const struct {
  const uint16_t *ptr;
  const uint32_t *size;
} rtc_fncs[] = {
  { patch_rtc_probe,       &patch_rtc_probe_size },
  { patch_rtc_reset,       &patch_rtc_reset_size },
  { patch_rtc_getstatus,   &patch_rtc_getstatus_size },
  { patch_rtc_gettimedate, &patch_rtc_gettimedate_size },
};


// Flash patching functions per-mode.

#define FN_THUMB_RET0     0x47702000
#define FN_THUMB_RET1     0x47702001
#define FN_ARM_RET0       0xe3a00000
#define FN_ARM_RET1       0xe3a00001
#define FN_ARM_RETBX      0xe12fff1e

static void apply_patch_ops(t_rom_part *rp, const uint32_t *ops, unsigned pcount,
                            const t_patch_prog *prgs, const t_psave_info *psi) {
  for (unsigned i = 0; i < pcount; i++) {
    uint32_t opc = ops[i] >> 28;
    uint32_t arg = (ops[i] >> 25) & 7;
    uint32_t moff = ops[i] & 0x1FFFFFF;

    switch (opc) {
    case 0x0:
      // Patch a full program into an address.
      for (unsigned j = 0; j < prgs[arg].length; j++)
        put8(rp, moff + j, prgs[arg].data[j]);
      break;
    case 0x1:   // Patch Thumb instruction
      put16(rp, moff, 0x46C0);       // mov r8, r8
      break;
    case 0x2:   // Patch ARM instruction
      put32(rp, moff, 0xE1A00000);   // mov r0, r0
      break;
    case 0x3:   // Write N bytes to address (they follow the op)
      for (unsigned j = 0; j < arg + 1; j++)
        put8(rp, moff + j, ops[i + 1 + j / 4] >> ((j % 4) * 8));
      i += (arg + 1 + 3) / 4;
      break;
    case 0x4:   // Write N words to address (they follow the op)
      for (unsigned j = 0; j < arg + 1; j++)
        put32(rp, moff + j * 4, ops[++i]);
      break;
    case 0x5:   // Patch function with a dummy one
      switch (arg) {
        case 0:
        case 1:
          put32(rp, moff, arg ? FN_THUMB_RET1 : FN_THUMB_RET0);
          break;
        case 4:
        case 5:
          put32(rp, moff, (arg == 5) ? FN_ARM_RET1 : FN_ARM_RET0);
          put32(rp, moff + 4, FN_ARM_RETBX);
          break;
      };
      break;

    case 0x7:    // RTC handlers
      copy_func16(rp, moff, rtc_fncs[arg].ptr, *rtc_fncs[arg].size);
      break;

    case 0x8:    // EEPROM memory handlers
      if (arg < 2) {
        unsigned fnsz = *psi->sfns->eeprom_fncs[arg].size;
        copy_func16(rp, moff, psi->sfns->eeprom_fncs[arg].ptr, fnsz);
        put32(rp, moff + fnsz, psi->dspayload_addr);
      }
      break;

    case 0x9:    // FLASH memory handlers
      if (arg < 5) {
        unsigned fnsz = *psi->sfns->flash_fncs[arg].size;
        copy_func16(rp, moff, psi->sfns->flash_fncs[arg].ptr, fnsz);
        put32(rp, moff + fnsz, psi->dspayload_addr);
      }
    };
  }
}


// Applies a patch directly into the ROM memory. The ROM can be a partial image
// (i.e. half a ROM or similar) but it should always be 4byte aligned (size too).
// Assuming we do at least 512 byte blocks or so too. False if a write never
// read back as written.
bool patch_apply_rom(
  // Where the ROM has been loaded.
  uint8_t *buffer, unsigned bufsize,
  // What base address this ROM has.
  uint32_t baseaddr,
  // Patching options
  bool patch_waitcnt,
  const t_patch *pdata,
  bool patch_rtc,
  uint32_t igmenu_addr,
  uint32_t ds_addr
) {
  t_rom_part rp = { buffer, baseaddr, bufsize, false };
  const uint32_t *ops = &pdata->op[0];
  // Save patch routines vary depending on whether DirectSave is enabled or not.
  const t_psave_info psi = {
    .dspayload_addr = ds_addr,
    .sfns = ds_addr                                ? &pdirectsave :
            pdata->save_mode == SaveTypeFlash1024K ? &psram_conversion_128k
                                                   : &psram_conversion_64k
  };

  // Apply the WAIT CNT patches
  if (patch_waitcnt)
    apply_patch_ops(&rp, ops, pdata->wcnt_ops, pdata->prgs, &psi);
  ops += pdata->wcnt_ops;

  // Apply save patches
  apply_patch_ops(&rp, ops, pdata->save_ops, pdata->prgs, &psi);
  ops += pdata->save_ops;

  // Apply optional patches, they are placed right after.
  if (igmenu_addr) {
    apply_patch_ops(&rp, ops, pdata->irqh_ops, pdata->prgs, &psi);

    // Need to patch the header with some entrypoint detour.
    if (baseaddr == 0x0) {
      uint32_t ibranch = *(uint32_t*)buffer;
      uint32_t boot_addr = ((ibranch & 0xFFFFFF) << 2) + 8 + GBA_ROM_BASE;

      // Calculate the branch from 0x08000000 to igmenu_addr
      unsigned brop = 0xEA000000 | ((igmenu_addr - GBA_ROM_BASE - 8) >> 2);

      // Patch the first instruction with the branch opcode
      put32(&rp, 0, brop);
      // Patch offset 0xB8 (unused header bits) with the real boot addr.
      put32(&rp, 0xB8, boot_addr);
    }
  }
  ops += pdata->irqh_ops;

  // Apply RTC patches
  if (patch_rtc)
    apply_patch_ops(&rp, ops, pdata->rtc_ops, pdata->prgs, &psi);

  return !rp.failed;
}

// Applies a payload to the ROM memory. False if a write never read back as
// written.
bool payload_apply_rom(
  // Where the ROM has been loaded.
  uint8_t *buffer, unsigned bufsize,
  // What base address this ROM has.
  uint32_t baseaddr,
  // Patch buffer to apply and its offset.
  const uint8_t *payload, unsigned payload_size,
  uint32_t payload_offset
) {
  // Optimize away the copy call if this can't possibly overlap.
  if (payload_offset >= baseaddr + bufsize || baseaddr >= payload_offset + payload_size)
    return true;

  t_rom_part rp = { buffer, baseaddr, bufsize, false };
  copy_func16(&rp, payload_offset, payload, payload_size);
  return !rp.failed;
}

