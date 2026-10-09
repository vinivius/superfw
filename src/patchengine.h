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

#ifndef _PATCHENGINE_H_
#define _PATCHENGINE_H_

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "fatfs/ff.h"
#include "common.h"

#define MAX_PATCH_OPS           128   // (artifically limited to save memory)
#define MAX_PATCH_PRG             4   // Only 4 programs can be encoded so far

typedef struct {
  uint32_t length;
  uint8_t data[60];
} t_patch_prog;

struct struct_t_patch {
  uint8_t wcnt_ops;               // WaitCNT patches
  uint8_t save_ops;               // Save patches
  uint8_t save_mode;              // Save mode type (memory type)
  uint8_t irqh_ops;               // IRQ handler patches
  uint8_t rtc_ops;                // RTC patches
  uint32_t hole_size;             // Hole/trailing info, for ROM free space
  uint32_t hole_addr;
  uint32_t op[MAX_PATCH_OPS];     // Contain patch info for waitcnt and save
  t_patch_prog prgs[MAX_PATCH_PRG];
};

typedef struct struct_t_patch t_patch;

typedef struct {
  unsigned filesize;
  // Save type related info
  unsigned save_type_guess;
  unsigned flash64cnt, flash128cnt;
  // RTC stuff
  bool rtc_guess;
  // Trailing data
  uint32_t ldata, ldatacnt;
  // Save ops found (the ones of the save type found are kept at the end)
  uint32_t save_op[MAX_PATCH_OPS];
  unsigned save_cnt;
  uint16_t save_lost;             // Save types (bit OPC_*_HD) with more ops than save_op holds
  // More ops found than a patch holds (MAX_PATCH_OPS): unusable
  bool overflow;
  // The actual patch data.
  t_patch p;
} t_patch_builder;

void patchmem_dbinfo(const uint8_t *dbptr, uint32_t *pcnt, char *version, char *date, char *creator);
// Lookup routines (builtin, on-disk, etc).
bool patchmem_lookup(const uint8_t *gamecode, const uint8_t *dbptr, t_patch *pdata);
// Whether the database at dbptr is one this firmware uses (signature, version).
bool patchmem_valid(const uint8_t *dbptr);
// Actual patching magic
bool patch_apply_rom(uint8_t *buffer, unsigned bufsize, uint32_t baseaddr, bool patch_waitcnt, const t_patch *pdata,
                     bool patch_rtc, uint32_t igmenu_addr, uint32_t ds_addr);
// Payload patching routine
bool payload_apply_rom(uint8_t *buffer, unsigned bufsize, uint32_t baseaddr,
                       const uint8_t *payload, unsigned payload_size, uint32_t payload_offset);

void patchengine_init(t_patch_builder *patch, unsigned filesize);
void patchengine_finalize(t_patch_builder *patch);
// Generates a patch set from a given ROM.
// Scans the words rom[first .. first + count) of a ROM (rom[0] is at byte base
// in it): a chunk of it, with the PE_LOOKBACK bytes before it (LDR searches)
// and PE_LOOKAHEAD after it (signatures) loaded around it, as far as they're
// in the ROM (zeros past its end).
#define PE_LOOKBACK      8192
#define PE_LOOKAHEAD     4096
void patchengine_process_rom(const uint32_t *rom, unsigned first, unsigned count, uint32_t base,
                             t_patch_builder *patch, void(*progresscb)(unsigned));

// A ROM of romsize bytes is scanned in chunks of up to chunk bytes, the one at
// off loaded from start to end (its context, zeros past the ROM's end; whole
// 4KiB blocks) and scanned from word first, count words.
typedef struct {
  unsigned size;                  // Its bytes of the ROM, from the offset
  unsigned start, end;            // The ROM bytes it needs (end past the ROM: zeros)
  unsigned first, count;          // Its words to scan, from start
} t_pe_chunk;
void patchengine_chunk(unsigned romsize, unsigned off, unsigned chunk, t_pe_chunk *c);

// Tries to load patches from disk
// The patch file of a ROM (next to it, or generated): see load_patch_file().
FRESULT load_cached_patches(const char *romfn, const t_game_id *id, unsigned romfs, t_patch *patches);
FRESULT load_rom_patches(const char *romfn, t_patch *patches);
// Saves the patches to disk
bool write_patches_cache(const char *romfn, const t_game_id *id, const t_patch *patches);
void drop_patches_cache(const char *romfn);

int serialize_patch(const t_patch *patch, uint8_t *buffer);
bool unserialize_patch(const uint8_t *buffer, unsigned size, t_patch *patch);
bool patch_check(const t_patch *patch, int *v1tables);


#endif

