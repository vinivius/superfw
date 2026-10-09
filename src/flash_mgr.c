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

#pragma GCC optimize("Os")
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "flash_mgr.h"
#include "flash.h"
#include "util.h"

// Flash management routines
//
// A big flash memory with mappeable regions exists and can be used to write
// and load game ROMs.
// Some flash region is devoted to metadata storage. This allows to play
// regardless of the SD card files/state.

static uint32_t xorh(const uint32_t *p, unsigned wc) {
  uint32_t ret = 0;
  while (wc--)
    ret ^= *p++;
  return ret;
}

// The TOC is a log of whole tables (each store appends one). Walks its
// entries' headers: the newest TOC_RECENT ones (newest first, off -1 if none;
// recent may be NULL) and where the next one would go. False if it couldn't
// be read.
#define TOC_RECENT 4
typedef struct {
  int off;
  unsigned size;
} t_toc_pos;

static bool toc_walk(uint32_t baseaddr, unsigned maxsize, t_toc_pos *recent, unsigned *end) {
  if (recent)
    for (unsigned i = 0; i < TOC_RECENT; i++)
      recent[i].off = -1;
  unsigned off = 0;
  t_reg_entry hdr;
  while (off + sizeof(hdr) <= maxsize) {
    if (!flash_read(baseaddr + off, (uint8_t*)&hdr, sizeof(hdr)))
      return false;
    const unsigned esz = sizeof(t_reg_entry) + sizeof(t_flash_game_entry) * hdr.gamecnt;
    if (hdr.magic != NOR_ENTRY_MAGIC || hdr.gamecnt > FLASHG_MAXFN_CNT || off + esz > maxsize)
      break;
    if (recent) {
      memmove(&recent[1], &recent[0], (TOC_RECENT - 1) * sizeof(recent[0]));
      recent[0] = (t_toc_pos){ off, esz };
    }
    off += esz;
  }
  *end = off;
  return true;
}

static bool flashmgr_erase(uint32_t baseaddr, unsigned size) {
  // Ensure the flash has CFI and we know about block size.
  // Currently only homogeneous sector sizes are supported
  if (!flashinfo.size || !flashinfo.blksize || !flashinfo.blkcount || flashinfo.regioncnt != 1)
    return false;

  // The block size has to make sense for our purposes.
  if (size < flashinfo.blksize || (size % flashinfo.blksize != 0))
    return false;

  // Wipe the area block by block
  for (unsigned i = 0; i < size; i += flashinfo.blksize) {
    // Check if the sector is already cleared and skip it.
    if (flash_check_erased(baseaddr + i, flashinfo.blksize))
      continue;

    if (!flash_erase_sector(baseaddr + i))
      return false;
  }

  return true;
}

// Fills the newest valid TOC (a newer entry may be damaged, ie. a store cut
// short): 1 if loaded, 0 if there's none, -1 if it couldn't be read (into the
// cart's SDRAM).
int flashmgr_load(uint32_t baseaddr, unsigned maxsize, t_reg_entry *ndata) {
  t_toc_pos recent[TOC_RECENT];
  unsigned end;
  if (!toc_walk(baseaddr, maxsize, recent, &end))
    return -1;
  for (unsigned i = 0; i < TOC_RECENT && recent[i].off >= 0; i++) {
    if (!flash_read(baseaddr + recent[i].off, (uint8_t*)ndata, recent[i].size))
      return -1;
    if (flashmgr_check(ndata))
      return 1;
  }
  return 0;
}

bool flashmgr_check(const t_reg_entry *ndata) {
  // Check the checksum
  unsigned gsize = sizeof(t_flash_game_entry) * ndata->gamecnt;
  uint32_t crc = xorh((uint32_t*)ndata->games, gsize / 4) ^ ndata->gamecnt;
  if (crc != ndata->crc)
    return false;

  // Now check that the game block mapping is well formed.
  uint32_t blkm[BMSIZE(NOR_BLOCK_COUNT, uint32_t)] = {0};

  for (unsigned i = 0; i < ndata->gamecnt; i++) {
    for (unsigned j = 0; j < MAX_GAME_BLOCKS; j++) {
      uint8_t n = ndata->games[i].blkmap[j];
      if (n) {
        if (n >= NOR_BLOCK_COUNT || BM_TEST(blkm, n))
          return false;      // No such block, or used twice!
        BM_SET(blkm, n);
      }
    }
    // Its name ends (it's used as a string).
    if (!memchr(ndata->games[i].game_name, 0, sizeof(ndata->games[i].game_name)))
      return false;
  }

  return true;
}

// Appends some new entries to the metada flash block.
bool flashmgr_store(uint32_t baseaddr, unsigned maxsize, t_reg_entry *ndata) {
  const unsigned reqsz = sizeof(t_reg_entry) + sizeof(t_flash_game_entry) * ndata->gamecnt;

  // It goes after the last entry if it fits there and that space is erased
  // (a store cut short leaves data). Else the area is wiped (the table
  // written is whole): flash looks bogus, or is full.
  unsigned off;
  if (!toc_walk(baseaddr, maxsize, NULL, &off))
    return false;            // Couldn't be read: it isn't wiped
  if (off + reqsz > maxsize ||
      !flash_check_erased(baseaddr + off, MIN(ROUND_UP2(reqsz, 32), maxsize - off))) {
    if (!flashmgr_erase(baseaddr, maxsize))
      return false;

    off = 0;  // Start writing at the top now that it's empty.
  }

  // The table stored is the caller's, its wear counters too: its magic and
  // checksum are set (checked: ndata is in the cart's SDRAM).
  const uint32_t id[2] = {
    NOR_ENTRY_MAGIC,
    xorh((uint32_t*)ndata->games, (sizeof(t_flash_game_entry) * ndata->gamecnt) / 4) ^ ndata->gamecnt
  };
  if (!memcpy32_checked(&ndata->magic, id, sizeof(id)))
    return false;

  if (!flash_program(baseaddr + off, (uint8_t*)ndata, reqsz))
    return false;

  if (!flash_verify(baseaddr + off, (uint8_t*)ndata, reqsz))
    return false;

  return true;
}

// Wipes flash metadata block.
bool flashmgr_wipe(uint32_t baseaddr, unsigned maxsize) {
  return flashmgr_erase(baseaddr, maxsize);
}

// Allocates blocks based on wear and updates write cycles information.
bool flashmgr_allocate_blocks(uint8_t *blockmap, unsigned nalloc, t_reg_entry *ndata) {
  // Check which blocks are free and allocate some free blocks to use.
  uint32_t blkm[BMSIZE(NOR_BLOCK_COUNT, uint32_t)] = {0};
  for (unsigned i = 0; i < ndata->gamecnt; i++)
    for (unsigned j = 0; j < MAX_GAME_BLOCKS; j++)
      BM_SET(blkm, ndata->games[i].blkmap[j]);

  // Allocate blocks prioritizing blocks with less write cycles.
  for (unsigned a = 0; a < nalloc; a++) {
    uint8_t cand = 0;
    uint32_t wrcyc = ~0U;
    for (unsigned i = 1; i < NOR_BLOCK_COUNT; i++) {
      if (!BM_TEST(blkm, i) && ndata->wr_cycles[i] < wrcyc) {
        cand = i;
        wrcyc = ndata->wr_cycles[i];
      }
    }

    if (!cand)
      return false;

    // Mark the block as used, book it and increase cycle count.
    BM_SET(blkm, cand);
    blockmap[a] = cand;
    const uint32_t cyc = ndata->wr_cycles[cand] + 1;
    if (!memcpy32_checked(&ndata->wr_cycles[cand], &cyc, sizeof(cyc)))
      return false;
  }

  return true;
}

