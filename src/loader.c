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

#pragma GCC optimize("Os")
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gbahw.h"
#include "compiler.h"
#include "save.h"
#include "patchengine.h"
#include "settings.h"
#include "ingame.h"
#include "fonts/font_render.h"
#include "supercard_driver.h"
#include "fatfs/ff.h"
#include "directsave.h"
#include "common.h"
#include "util.h"
#include "flash_mgr.h"
#include "flash.h"

// Here we have the ROM loading routines.

#define LOAD_BS     (8*1024)     // Load in 8KB chunks

#define GBA_ROM_ADDR                  ((uint8_t *)0x08000000)
#define GBA_ROM_ADDR16(addr, value)   *((volatile uint16_t *)(0x08000000 + addr)) = (value)
#define GBA_ROM_ADDR32(addr, value)   *((volatile uint32_t *)(0x08000000 + addr)) = (value)

// The firmware block (#0) is mapped to the last 4MiB block, plus its offset.
#define FLASH_DIRSAV_PAYLOAD_W0       (0x0A000000 - NOR_BLOCK_SIZE + 0x00190000)
#define FLASH_IGM_TRAMPOLINE_W0       (0x0A000000 - NOR_BLOCK_SIZE + 0x00198000)

#define ING_PALETTE_BASE    240

extern bool slowsd;

bool validate_gba_header(const uint8_t *header) {
  const t_rom_header *gbah = (t_rom_header*)header;

  // Check that the checksum is OK:
  uint8_t checksum = 0x19;
  for (unsigned i = 0xA0; i < 0xBD; i++)
    checksum += header[i];
  checksum = -checksum;
  if (checksum != gbah->checksum)
    return false;

  // Misc header stuff
  if (header[0xb2] != 0x96)
    return false;

  // Validate the nintendo logo as well
  uint32_t ck = 0;
  for (unsigned i = 0; i < 39; i++)
    ck ^= gbah->logo_data[i];
  if (ck != 0xf8cff8fc)
    return false;

  return true;
}

bool validate_gb_header(const uint8_t *header) {
  // Check that the checksum is OK:
  const t_gbheader* hdr = (t_gbheader*)header;
  uint8_t checksum = 0;
  for (unsigned i = 0x34; i <= 0x4C; i++)
    checksum = checksum - header[i] - 1;
  if (checksum != hdr->checksum)
    return false;

  const uint32_t *logo32 = (uint32_t*)hdr->logo_data;
  uint32_t logocheck = 0;
  for (unsigned i = 0; i < 12; i++)
    logocheck ^= logo32[i];
  if (logocheck != 0x83e1df3b)
    return false;

  return true;
}

void fix_gba_header(volatile uint16_t *header) {
  // Fix header checksum unconditionally.
  header[0xB2 / 2] = 0x0096;     // Device ID/fixed value
  header[0xB4 / 2] = 0x0000;
  header[0xB6 / 2] = 0x0000;
  // 0xB8 and 0xBA are left out (contain IGM information)
  header[0xBC / 2] = header[0xBC / 2] & 0xFF; // Preserve version num

  uint8_t crc = header[0xBC / 2];
  for (unsigned i = 0; i < 14; i++) {
    uint16_t v = header[0xA0 / 2 + i];
    crc += (v & 0xFF) + (v >> 8);
  }

  header[0xBC / 2] = (-(0x19+crc)) << 8 | header[0xBC / 2];
}

// Loads the in-game menu at the desired address and size (returns success).
// addr must be 4 byte aligned.
void load_ingame_menu(
  uint32_t base_addr, uint32_t total_size, bool useds,
  const char* savefn, const char* statefn,
  bool rtc_patches, unsigned cheats
) {
  const unsigned menu_size = ingame_menu_payload.menu_rsize;
  const unsigned fontsz = font_block_size();

  set_supercard_mode(MAPPED_SDRAM, true, false);

  // Copy the font pack first, using memmove to handle collisions properly.
  // TODO: Allow partial font copying, to reduce memory usage (ie. in 32MiB ROMs)
  uint8_t *ptr = (uint8_t*)base_addr;
  uint8_t *font_ptr = (uint8_t*)ROM_FONTBASE_U8;
  // Copy fonts, and cheats (appended right after the cheat buffer)
  memmove32(&ptr[menu_size], font_ptr, fontsz + cheats);

  // Copy the in-game-menu payload from rodata
  t_igmenu *igm = (t_igmenu*)base_addr;
  memcpy32(ptr, &ingame_menu_payload, ingame_menu_payload_size);

  // Decode the start instruction, calculate branch target.
  uint16_t hotk = hotkey_list[hotkey_combo].mask;
  set_abort_lr(hotk << 16);        // Write key to register as well

  // Patch the menu header to provide necessary data.
  igm->drv_issdhc = sc_issdhc();              // SD driver data (no init happens)
  igm->drv_rca = sc_rca();
  igm->menu_hotkey = hotk;                    // Configured hotkey
  igm->menu_lang = lang_id;                   // Use the current lang id
  igm->menu_use_directsave = useds;           // DirectSave in use
  igm->menu_anim_speed = anim_speed;          // Menu animation speed
  igm->menu_font_base = base_addr + menu_size;// Base addr where the fonts live
  igm->menu_cheats_base = cheats ? base_addr + menu_size + fontsz : 0;
  igm->scratch_space_base = base_addr + menu_size + fontsz + cheats;
  igm->scratch_space_size = total_size - (menu_size + fontsz + cheats);
  igm->menu_has_rtc_support = rtc_patches;    // Using RTC patches
  igm->savefile_backups = backup_sram_default;// Backup count
  for (unsigned i = 0; i < sizeof(igm->menu_palette) / sizeof(igm->menu_palette[0]); i++)
    igm->menu_palette[i] = MEM_PALETTE[ING_PALETTE_BASE + i];

  // Calculate the basename, so we can produce proper sav/backup files
  // Only used if saving is enabled and DirSav is disabled.
  if (savefn && !useds) {
    char save_basename[MAX_FN_LEN];
    strcpy(save_basename, savefn);
    replace_extension(save_basename, "");
    memcpy32(igm->savefile_pattern, save_basename, sizeof(igm->savefile_pattern));
  }
  else
    memset32(igm->savefile_pattern, 0, sizeof(igm->savefile_pattern));

  memcpy32(igm->statefile_pattern, statefn, sizeof(igm->statefile_pattern));

  set_supercard_mode(MAPPED_SDRAM, true, true);
}

void load_directsave_config(const t_dirsave_info *dsinfo) {
  t_dirsave_config cfg = {
    .magic = DIRSAV_CFG_MAGIC,
    .checksum = 0,
    .nrandom = 0xdeadbeef ^ systime(),
    .memory_size = dsinfo->save_size,
    .base_sector = dsinfo->sector_lba,
    .drv_issdhc = sc_issdhc(),
    .drv_rca = sc_rca(),
    .sd_mutex = 0
  };
  const uint32_t *cfg32 = (uint32_t*)(&cfg);
  uint32_t checksum = 0;
  for (unsigned i = 0; i < sizeof(cfg) / 4; i++)
    checksum ^= *cfg32++;
  cfg.checksum = checksum;

  write_sram_buffer((uint8_t*)(&cfg), 64*1024 - sizeof(cfg), sizeof(cfg));
}

void load_rtcclock_data(const t_rtc_info *rtcinfo) {
  set_undef_lrsp(rtcinfo->timestamp, rtcinfo->ts_step);
}

// Loads ROM header from disk for inspection.
unsigned preload_gba_rom(const char *fn, uint32_t fs, t_rom_header *romh) {
  FIL fd;
  FRESULT res = f_open(&fd, fn, FA_READ);
  if (res != FR_OK)
    return ERR_LOAD_BADROM;

  UINT rdbytes;
  bool err = (FR_OK != f_read(&fd, romh, sizeof(*romh), &rdbytes) || rdbytes != sizeof(*romh));

  f_close(&fd);
  return err ? ERR_LOAD_BADROM : 0;
}

uint32_t load_sdram_end = 0;
bool load_sdram_lost = false;

void load_sdram_reset(void) {
  load_sdram_end = 0;
  load_sdram_lost = false;
}

// Records that a load writes SDRAM [start, end).
static void load_writes(uint32_t start, uint32_t end) {
  if (start < ROM_OFF_FONTS_BASE)
    load_sdram_end = MAX(load_sdram_end, end);
  // The fonts and cheats, then (above the high scratch area) the patch
  // databases and the bundled emulators.
  if ((start < ROM_OFF_HISCRATCH && end > ROM_OFF_FONTS_BASE) || end > ROM_OFF_USRPATCH_DB)
    load_sdram_lost = true;
}

// Data written to SDRAM is read back and rewritten if it did not stick (some
// carts occasionally drop SDRAM writes), up to CHUNK_WRITE_TRIES times. The
// chunks that needed it are counted (and logged) for diagnostics.
#define CHUNK_WRITE_TRIES   8
static unsigned chunk_rewrites = 0;
#ifdef HAVE_LOGGING
  static uint32_t chunk_rewrite_off[16];
  static uint8_t chunk_rewrite_cnt[16];
  static unsigned chunk_rewrite_num = 0;
#endif

static void chunk_stats_reset() {
  chunk_rewrites = 0;
  #ifdef HAVE_LOGGING
    chunk_rewrite_num = 0;
  #endif
}

// Records the extra writes a chunk needed (-1: it never verified).
static void chunk_rewritten(uint32_t offset, int extra) {
  if (!extra)
    return;
  chunk_rewrites++;
  #ifdef HAVE_LOGGING
  if (chunk_rewrite_num < 16) {
    chunk_rewrite_off[chunk_rewrite_num] = offset;
    chunk_rewrite_cnt[chunk_rewrite_num++] = extra < 0 ? 0xFF : extra;
  }
  #else
    (void)offset;
  #endif
}

#ifdef HAVE_LOGGING
static void log_chunk_rewrites(const char *what) {
  for (unsigned i = 0; i < chunk_rewrite_num; i++)
    WRITE_LOG("%s chunk at 0x%06lx needed rewriting (%u extra writes)",
              what, chunk_rewrite_off[i], chunk_rewrite_cnt[i]);
  if (chunk_rewrites)
    WRITE_LOG("%s chunks rewritten: %u", what, chunk_rewrites);
}
#else
  #define log_chunk_rewrites(what)
#endif

// Pads buf with zeros from len up to a word boundary, returns the new length.
static unsigned pad_to_word(void *buf, unsigned len) {
  unsigned padded = ROUND_UP2(len, 4);
  memset((uint8_t*)buf + len, 0, padded - len);
  return padded;
}

// Copies a loaded chunk to SDRAM at offset (from GBA_ROM_ADDR) as whole words
// (src is padded with zeros up to a word boundary) and reads it back,
// rewriting it as needed. ck is the running checksum of the data loaded so
// far (checksum_words()): the chunk is checked against it and added to it.
// Returns the number of extra writes needed, or -1 if it never verified.
NOINLINE
static int copy_chunk_verified(uint32_t offset, uint32_t *src, unsigned bytes, uint32_t *ck) {
  uint8_t *dst = &GBA_ROM_ADDR[offset];
  bytes = pad_to_word(src, bytes);
  if (!bytes)
    return 0;         // Nothing to copy (a DMA count of 0 would copy 64K words)
  load_writes(offset, offset + bytes);
  uint32_t ck_src[2] = {ck[0], ck[1]};
  checksum_words(src, bytes / 4, ck_src);

  set_supercard_mode(MAPPED_SDRAM, true, false);
  int ret = -1;
  for (unsigned t = 0; t < CHUNK_WRITE_TRIES; t++) {
    if (use_slowld)
      rom_copy_write16(dst, src, bytes);
    else
      dma_memcpy32(dst, src, bytes/4);

    uint32_t ck_dst[2] = {ck[0], ck[1]};
    checksum_words(dst, bytes / 4, ck_dst);
    if (ck_dst[0] == ck_src[0] && ck_dst[1] == ck_src[1]) {
      ret = t;
      break;
    }
  }
  set_supercard_mode(MAPPED_SDRAM, true, true);

  ck[0] = ck_src[0];
  ck[1] = ck_src[1];
  chunk_rewritten(offset, ret);
  return ret;
}

// Checksums ROM data already loaded in SDRAM (used to verify the load).
static void checksum_loaded_rom(uint32_t start, uint32_t end, uint32_t *st) {
  if (end <= start)
    return;
  // The SD interface overlaps the upper ROM area, unmap it while reading.
  // Whole words: the end of the file is padded with zeros in SDRAM.
  set_supercard_mode(MAPPED_SDRAM, true, false);
  checksum_words((const void*)(GBA_ROM_ADDR + start), (end - start + 3) / 4, st);
  set_supercard_mode(MAPPED_SDRAM, true, true);
}

// Loads the file region [start, end) to the same offsets in SDRAM, adding it
// to the running checksum ck; dry, it only checksums it (to read it again).
// Progress counts chunks in *steps, out of nsteps.
static unsigned load_rom_region(FIL *fd, uint32_t start, uint32_t end, uint32_t *ck, bool dry,
                                progress_fn progress, uint32_t *steps, uint32_t nsteps) {
  if (start >= end)
    return 0;
  if (FR_OK != f_lseek(fd, start))
    return ERR_LOAD_BADROM;
  for (uint32_t offset = start; offset < end; offset += LOAD_BS) {
    if (progress && (*steps & 31) == 0)
      progress(*steps, nsteps);
    (*steps)++;

    unsigned toread = MIN(LOAD_BS, end - offset);
    UINT rdbytes;
    uint32_t tmp[LOAD_BS/4];
    if (FR_OK != f_read(fd, tmp, toread, &rdbytes) || rdbytes != toread)
      return ERR_LOAD_BADROM;
    if (dry)
      checksum_words(tmp, pad_to_word(tmp, toread) / 4, ck);
    else if (copy_chunk_verified(offset, tmp, toread, ck) < 0) {
      WRITE_LOG("ROM chunk at 0x%06lx never verified in SDRAM", offset);
      return ERR_LOAD_VERIFY;
    }
  }
  return 0;
}

unsigned load_gba_rom(
  const char *fn, uint32_t fs,
  const char *savefn,
  const t_patch *ptch,
  const t_dirsave_info *dsinfo,
  bool ingame_menu,
  bool keep_igm,
  const t_rtc_info *rtcinfo,
  unsigned cheats,
  progress_fn progress
) {

  bool use_rtc_patches = rtcinfo != NULL;

  chunk_stats_reset();

  // Determine how much ROM space we need for the IGM and DirSav payloads
  const unsigned igm_reqsz = ingame_menu_payload.menu_rsize + font_block_size();
  WRITE_LOG("Load sizes: rom %lu, igm %u, fonts %u", fs, ingame_menu_payload.menu_rsize, font_block_size());
  // Round it up, reserve ~1KB after the ROM for patches.
  // 32MiB games cannot generate patches beyond the end.
  const unsigned romrsize = ROUND_UP2(fs, 1024) + (fs < MAX_GBA_ROM_SIZE ? 1024 : 0);
  // Required size for these payloads. We should always have enough, since menu checks it.
  const unsigned req_size = (ingame_menu ? igm_reqsz : 0) + (dsinfo ? DIRSAVE_REQ_SPACE : 0);

  uint32_t igm_addr, igm_space, ds_addr;
  if (romrsize + req_size <= MAX_GBA_ROM_SIZE) {
    // Allocate the DirSav payload first
    ds_addr = romrsize;
    // Now the IGM
    igm_addr = ds_addr + (dsinfo ? DIRSAVE_REQ_SPACE : 0);
    // Calculate the total space for the IGM to use
    igm_space = MAX_GBA_ROM_SIZE - igm_addr;
  }
  else {
    // Cannot append it at the end, it's too big. Check if we have a hole.
    if (!ptch || ptch->hole_size < req_size)
      return ERR_NO_PAYLOAD_SPACE;

    ds_addr = ptch->hole_addr;
    igm_addr = ds_addr + (dsinfo ? DIRSAVE_REQ_SPACE : 0);
    igm_space = ptch->hole_size - (dsinfo ? DIRSAVE_REQ_SPACE : 0);
  }
  // Round it down to a KB boundary
  igm_space &= ~1023;

  // Calculate the "hole" limits
  uint32_t gap_start = ds_addr;
  uint32_t gap_end = igm_addr + igm_space;

  // keep_igm: a retry of a load that overwrote the fonts and cheats the
  // in-game menu is made from keeps the menu that load installed (the ROM
  // load skips its space).
  const bool install_igm = ingame_menu && !keep_igm;
  if (ingame_menu)
    load_writes(igm_addr, igm_addr + igm_reqsz + cheats);

  // Get aboslute addresses
  ds_addr += GBA_ROM_BASE;
  igm_addr += GBA_ROM_BASE;

  // Install the menu before loading the ROM, otherwise we overwrite relevant assets.
  if (install_igm) {
    char sfn[MAX_FN_LEN];
    savestate_filename_calc(fn, sfn);
    load_ingame_menu(igm_addr, igm_space, dsinfo, savefn, sfn, use_rtc_patches, cheats);
  }
  else if (ingame_menu) {
    // Kept from the first try: the SD card may have been set up again since.
    t_igmenu *igm = (t_igmenu*)igm_addr;
    set_supercard_mode(MAPPED_SDRAM, true, false);
    igm->drv_issdhc = sc_issdhc();
    igm->drv_rca = sc_rca();
    set_supercard_mode(MAPPED_SDRAM, true, true);
  }

  // Proceed to load the ROM
  FIL fd;
  FRESULT res = f_open(&fd, fn, FA_READ);
  if (res != FR_OK)
    return ERR_LOAD_BADROM;

  // Calculate progress bar steps, carefully consider the gap (if any).
  const uint32_t load_steps = (gap_end <= fs  ? (fs - (gap_end - gap_start)) :
                               gap_start < fs ? (fs - gap_start)             : fs) / LOAD_BS;
  uint32_t steps = 0;

  // Honor fast loading (switch mirror if appropriate)
  slowsd = use_slowld;

  // The file data goes to [0, gap_start) and [gap_end, fs). Its checksum as
  // read from the SD card verifies the SDRAM copy and, with ROM verification,
  // a second read of the file.
  const uint32_t seg1_end = MIN(gap_start, fs);
  uint32_t ck_load[2] = {0, 0}, ck[2] = {0, 0};
  unsigned err = load_rom_region(&fd, 0, seg1_end, ck_load, false, progress, &steps, load_steps);
  if (!err)
    err = load_rom_region(&fd, gap_end, fs, ck_load, false, progress, &steps, load_steps);
  if (!err) {
    progress(1, 1);  // Mark as complete
    log_chunk_rewrites("ROM");

    // Verify the load (before patching, which modifies the ROM).
    checksum_loaded_rom(0, seg1_end, ck);
    checksum_loaded_rom(gap_end, fs, ck);
    if (ck[0] != ck_load[0] || ck[1] != ck_load[1]) {
      WRITE_LOG("ROM verify: SDRAM copy mismatch");
      err = ERR_LOAD_VERIFY;
    }
  }
  if (!err && use_verify_rom) {
    // Read the file again, to catch corrupted reads from the SD card.
    ck[0] = ck[1] = 0;
    err = load_rom_region(&fd, 0, seg1_end, ck, true, NULL, &steps, 0);
    if (!err)
      err = load_rom_region(&fd, gap_end, fs, ck, true, NULL, &steps, 0);
    if (!err && (ck[0] != ck_load[0] || ck[1] != ck_load[1])) {
      WRITE_LOG("ROM verify: SD re-read mismatch");
      err = ERR_LOAD_VERIFY;
    }
  }
  if (err) {
    slowsd = true;
    f_close(&fd);
    return err;
  }

  WRITE_LOG("ROM verify OK (%08lx:%08lx, %lu bytes, re-read %u)", ck_load[0], ck_load[1], fs, use_verify_rom);

  slowsd = true;

  // Close the file, not super necessary really :P
  f_close(&fd);

  // Proceed to patch the ROM
  set_supercard_mode(MAPPED_SDRAM, true, false);

  // Load/Patch the DirectSave payload if necessary.
  if (dsinfo)
    payload_apply_rom(GBA_ROM_ADDR, MAX_GBA_ROM_SIZE, GBA_ROM_BASE, directsave_payload, directsave_payload_size, ds_addr);

  // Actually apply patches
  if (ptch) {
    patch_apply_rom(GBA_ROM_ADDR, MAX_GBA_ROM_SIZE, 0, true, ptch, use_rtc_patches,
                    ingame_menu ? igm_addr : 0, dsinfo ? ds_addr : 0);
    if (rtcinfo)
      load_rtcclock_data(rtcinfo);
  }

  // Fix header checksum unconditionally (just in case we boot to BIOS).
  fix_gba_header((uint16_t*)GBA_ROM_ADDR);

  // Go ahead and load config in SRAM, if applicable.
  if (dsinfo)
    load_directsave_config(dsinfo);

  // Set the ROM into read only mode, disable SD card reader as well. Maps SRAM bank 0.
  set_supercard_mode(MAPPED_SDRAM, false, false);

  REG_WAITCNT = 0x4000;
  launch_reset(boot_bios_splash, use_fastew);

  return 0;
}

// Flashes a game to NOR patching it as necessary. This includes DirSav as well as IGM.
NOINLINE
unsigned flash_gba_nor(
  const char *fn, uint32_t fs,
  const t_rom_header *rom_header,
  const t_patch *ptch,
  bool dirsaving,
  bool ingame_menu,
  bool rtc_patches,
  const uint8_t *blkmap,
  progress_fn progress,
  uint8_t *scratch, unsigned ssize
) {
  if (!flashinfo.size || !flashinfo.blksize || !flashinfo.blkcount || !flashinfo.blkwrite || !flashinfo.blksize)
    return ERR_FLASH_OP;

  // Determine if the DirSav payload must be flashed in a ROM gap or not.
  const bool flashmap0 = fs <= MAX_GBA_ROM_SIZE - NOR_BLOCK_SIZE;
  uint32_t ds_flashoffset  = flashmap0 ? 0 : ptch->hole_addr;
  uint32_t igm_flashoffset = flashmap0 ? 0 : ptch->hole_addr + DIRSAVE_REQ_SPACE;

  // Calculate where the DirSav / IGM is (flash mapped block0 or flashed on top of the game).
  uint32_t dsaddr  =  ds_flashoffset ? 0x08000000 +  ds_flashoffset : FLASH_DIRSAV_PAYLOAD_W0;
  uint32_t igmaddr = igm_flashoffset ? 0x08000000 + igm_flashoffset : FLASH_IGM_TRAMPOLINE_W0;

  FIL fd;
  FRESULT res = f_open(&fd, fn, FA_READ);
  if (res != FR_OK)
    return ERR_LOAD_BADROM;

  // Map the game to the base 32MiB address space.
  set_superchis_normap(blkmap);

  t_flash_erase_state erst;
  for (uint32_t bigoff = 0; bigoff < fs; bigoff += ssize) {
    // Start clearing the flash block we will be writing to!
    flash_erase_fsm_start(&erst, GBA_ROM_BASE_WS1 + bigoff, flashinfo.blksize, ssize / flashinfo.blksize);

    // Load the ROM in chunks to the scratch area. So that we can mange it.
    for (uint32_t offset = 0; offset < ssize && offset + bigoff < fs; offset += LOAD_BS) {
      uint32_t absoff = offset + bigoff;
      if ((absoff & (128*1024-1)) == 0) {
        if (progress)
          progress((bigoff + offset / 4) >> 8, fs >> 8);

        flash_erase_fsm_step(&erst);
      }

      unsigned toread = MIN(LOAD_BS, fs - absoff);
      UINT rdbytes;
      uint32_t tmp[LOAD_BS/4];
      if (FR_OK != f_read(&fd, tmp, toread, &rdbytes) || rdbytes != toread) {
        f_close(&fd);
        reset_superchis_normap();
        return ERR_LOAD_BADROM;
      }

      // Whole words: the end of the file is padded with zeros.
      dma_memcpy32(&scratch[offset], tmp, pad_to_word(tmp, toread) / 4);
    }

    // Patch ROM, don't need WAITCNT patches
    patch_apply_rom(scratch, ssize, bigoff, false, ptch, rtc_patches, ingame_menu ? igmaddr : 0, dirsaving ? dsaddr : 0);

    // Copy (partial) DirSav / IGM trampoline payloads if necessary
    if (dirsaving && ds_flashoffset)
      payload_apply_rom(scratch, ssize, bigoff, directsave_payload, directsave_payload_size, ds_flashoffset);

    if (ingame_menu && igm_flashoffset)
      payload_apply_rom(scratch, ssize, bigoff, ingame_trampoline_payload, ingame_trampoline_payload_size, igm_flashoffset);

    // Wait until erasing is complete (if it didn't complete in the meantime)
    while (1) {
      int r = flash_erase_fsm_step(&erst);
      if (r > 0)
        break;
      if (r < 0) {
        f_close(&fd);
        reset_superchis_normap();
        return ERR_FLASH_OP;
      }
    }

    // Write flash blocks.
    for (uint32_t offset = 0; offset < ssize && offset + bigoff < fs; offset += flashinfo.blksize) {
      uint32_t absoff = offset + bigoff;
      uint32_t flashaddr = GBA_ROM_BASE_WS1 + absoff;
      if (progress && (absoff & (128*1024-1)) == 0)
        progress((bigoff + ssize / 4 + offset * 3/4) >> 8, fs >> 8);

      unsigned toflash = MIN(flashinfo.blksize, ROUND_UP2(fs, 4) - absoff);
      bool wr_ok = flash_program_buffered(flashaddr, &scratch[offset], toflash, flashinfo.blkwrite);

      // Check the written block if so configured
      if (wr_ok && use_verify_nor)
        wr_ok = flash_verify(flashaddr, &scratch[offset], toflash);

      if (!wr_ok) {
        f_close(&fd);
        reset_superchis_normap();
        return ERR_FLASH_OP;
      }
    }
  }

  reset_superchis_normap();
  return 0;
}

NOINLINE
unsigned launch_gba_nor(
  const char *romfn, const char *savefn,
  const uint8_t *normap, unsigned blkcnts,
  const t_dirsave_info *dsinfo,
  const t_rtc_info *rtcinfo,
  bool ingame_menu,
  unsigned cheats
) {

  bool use_rtc_patches = rtcinfo != NULL;

  // Now the IGM: it sits along in the SDRAM (at 0x0 offset)
  uint32_t igm_addr = GBA_ROM_BASE;
  // IGM can use as much SDRAM as it needs, use 16MiB for now
  uint32_t igm_space = 16*1024*1024;

  // Install the menu before loading the ROM, otherwise we overwrite relevant assets.
  if (ingame_menu) {
    char sfn[MAX_FN_LEN];
    savestate_filename_calc(romfn, sfn);
    load_ingame_menu(igm_addr, igm_space, dsinfo, savefn, sfn, use_rtc_patches, cheats);
  }

  if (dsinfo)
    load_directsave_config(dsinfo);

  if (rtcinfo)
    load_rtcclock_data(rtcinfo);

  // Map the game NOR blocks. Unused blocks are zero mapped (to firmware)
  set_superchis_normap(normap);

  // Set the ROM into read only mode, disable SD card reader as well.
  set_supercard_mode(MAPPED_FIRMWARE, false, false);

  REG_WAITCNT = 0x4000;
  launch_reset(boot_bios_splash, use_fastew);

  return 0;
}

// Writes data into SDRAM (from GBA_ROM_ADDR on) as one stream of whole words,
// each one verified (copy_chunk_verified): up to 3 trailing bytes are carried
// over to the next write (or the flush), so data of any length can follow
// other data (ie. a ROM after an emulator whose size isn't a multiple of 4).
// The words are added to a running checksum, to verify the whole image once
// it is loaded.
typedef struct {
  uint32_t off;           // SDRAM offset of the next word
  uint32_t lim;           // Writes must end below this offset
  uint32_t ck[2];         // checksum_words() of the words written so far
  uint32_t carry;         // Bytes waiting for the next write
  unsigned ncarry;
  bool dry;               // Only checksum the data (to compare a second read)
} t_sdram_stream;

// Writes len bytes from buf, which must have 4 spare bytes after them.
static unsigned stream_write(t_sdram_stream *s, uint32_t *buf, unsigned len) {
  if (s->ncarry) {
    // Shift the data up by the carried bytes, a word at a time (little
    // endian), and put them in front.
    const unsigned sh = s->ncarry * 8;
    for (unsigned i = (len + s->ncarry - 1) / 4; i; i--)
      buf[i] = (buf[i] << sh) | (buf[i - 1] >> (32 - sh));
    buf[0] = (buf[0] << sh) | (s->carry & ((1U << sh) - 1));
    len += s->ncarry;
  }
  unsigned wlen = len & ~3;
  s->ncarry = len & 3;
  memcpy(&s->carry, &((uint8_t*)buf)[wlen], s->ncarry);
  if (!wlen)
    return 0;

  if (s->off + wlen > s->lim)
    return ERR_LOAD_TOOBIG;
  if (s->dry)
    checksum_words(buf, wlen / 4, s->ck);
  else if (copy_chunk_verified(s->off, buf, wlen, s->ck) < 0) {
    WRITE_LOG("ROM chunk at 0x%06lx never verified in SDRAM", s->off);
    return ERR_LOAD_VERIFY;
  }
  s->off += wlen;
  return 0;
}

// Writes out the carried bytes, padded to a word.
static unsigned stream_flush(t_sdram_stream *s) {
  uint32_t pad[2] = {0, 0};
  return stream_write(s, pad, (4 - s->ncarry) & 3);
}

// Streams the whole file fn. Returns 0, or the error: openerr if it doesn't
// exist, readerr if it can't be read (or a stream_write() one).
static unsigned stream_file(t_sdram_stream *s, const char *fn, unsigned openerr, unsigned readerr,
                            progress_fn progress, uint32_t fs) {
  FIL fd;
  FRESULT res = f_open(&fd, fn, FA_READ);
  if (res != FR_OK)
    return (res == FR_NO_FILE || res == FR_NO_PATH) ? openerr : readerr;

  unsigned err = 0;
  for (uint32_t offset = 0; !err; offset += LOAD_BS) {
    if (progress && (offset & (64*1024-1)) == 0)
      progress(offset, fs);

    UINT rdbytes;
    uint32_t tmp[LOAD_BS/4 + 1];
    if (FR_OK != f_read(&fd, tmp, LOAD_BS, &rdbytes))
      err = readerr;
    else if (!rdbytes)
      break;
    else
      err = stream_write(s, tmp, rdbytes);
  }
  f_close(&fd);
  return err;
}

// Generic-emulator (ie. NES, SMS, ...) loader. The emulator, the ROM header it
// needs and the ROM are loaded one after the other, and verified like GBA
// ROMs: a dropped SDRAM write would corrupt the emulator or the game. They
// stay below the fonts, so the menu can recover if the load fails.
NOINLINE
unsigned load_extemu_rom(const char *fn, uint32_t fs, const t_emu_loader *ldinfo, progress_fn progress) {
  if (fs > 8*1024*1024)
    return ERR_LOAD_TOOBIG;

  t_sdram_stream s = { .lim = ROM_OFF_FONTS_BASE };
  chunk_stats_reset();
  char emupath[64] = "";

  // Try to find a valid and existing emulator.
  if (!memcmp(ldinfo->emu_name, "vfs:", 4)) {
    // Bundled emulator: unpack it straight into SDRAM. The unpacker reads its
    // own output back, so a dropped write spreads: check the result against
    // the checksum made at build time, and unpack it again if needed.
    set_supercard_mode(MAPPED_SDRAM, true, false);    // The assets overlap the SD interface
    const t_vfile *vf = get_vfile(&ldinfo->emu_name[4]);
    int t = 0;
    if (vf) {
      for (; t < CHUNK_WRITE_TRIES; t++) {
        s.off = upkr_unpack16(GBA_ROM_ADDR, vf->payload);
        load_writes(0, s.off);
        s.ck[0] = s.ck[1] = 0;
        checksum_words(GBA_ROM_ADDR, s.off / 4, s.ck);
        if (s.ck[0] == vf->ck[0] && s.ck[1] == vf->ck[1])
          break;
      }
    }
    set_supercard_mode(MAPPED_SDRAM, true, true);
    if (!vf)
      return ERR_LOAD_NOEMU;    // Not bundled in
    chunk_rewritten(0, t < CHUNK_WRITE_TRIES ? t : -1);
    if (t == CHUNK_WRITE_TRIES) {
      WRITE_LOG("Emulator unpack mismatch");
      return ERR_LOAD_EMUERR;
    }
  }
  else {
    strcpy(emupath, EMULATORS_PATH);
    strcat(emupath, ldinfo->emu_name);
    strcat(emupath, ".gba");
  }

  // The SD emulator (if any), the ROM header it needs (64 bytes at most) and
  // the ROM follow. A second, dry pass reads them again (only checksumming)
  // with ROM verification, to catch corrupted reads from the SD card.
  const t_sdram_stream start = s;
  for (unsigned pass = 0; pass <= use_verify_rom; pass++) {
    t_sdram_stream st = start;
    st.dry = pass;
    unsigned err;
    if (emupath[0] && (err = stream_file(&st, emupath, pass ? ERR_LOAD_EMUERR : ERR_LOAD_NOEMU,
                                         ERR_LOAD_EMUERR, NULL, 0)))
      return err;
    if (ldinfo->hndlr) {
      uint32_t hdr[64/4 + 1];
      if ((err = stream_write(&st, hdr, ldinfo->hndlr((uint8_t*)hdr, fn, fs))))
        return err;
    }
    if ((err = stream_file(&st, fn, ERR_LOAD_BADROM, ERR_LOAD_BADROM, progress, fs)) ||
        (err = stream_flush(&st)))
      return err;

    // The first pass checks the whole image in SDRAM (a write can disturb data
    // written earlier), the second one that it reads the same.
    uint32_t ck[2] = {0, 0};
    if (!pass) {
      log_chunk_rewrites("Emulator load:");
      checksum_loaded_rom(0, st.off, ck);
      s = st;
    }
    else
      memcpy(ck, st.ck, sizeof(ck));
    if (ck[0] != s.ck[0] || ck[1] != s.ck[1]) {
      WRITE_LOG("Emulator load: %s mismatch", pass ? "SD re-read" : "SDRAM copy");
      return ERR_LOAD_VERIFY;
    }
  }

  // Set the ROM into read only mode, disable SD card reader as well.
  set_supercard_mode(MAPPED_SDRAM, false, false);

  REG_WAITCNT = 0x4000;   // Default timings + prefetch
  launch_reset(false, use_fastew);

  return 0;
}


