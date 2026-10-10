// Tests for the NOR game table log (flash_mgr.c) over a RAM flash.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "common.h"
#include "flash.h"
#include "flash_mgr.h"

#define BASE       0x100000
#define META_SIZE  (4 * 4096)       // A small log: 4 blocks of 4KiB

t_flash_info flashinfo = { .size = META_SIZE, .regioncnt = 1, .blksize = 4096, .blkcount = 4 };
static uint8_t flashmem[META_SIZE];
static uint32_t max_written;        // The end of the furthest write

bool flash_read(uint32_t addr, uint8_t *buf, unsigned size) {
  assert(addr >= BASE && addr + size <= BASE + META_SIZE);
  memcpy(buf, &flashmem[addr - BASE], size);
  return true;
}

bool flash_check_erased(uintptr_t addr, unsigned size) {
  size = size / 32 * 32;            // As check_erased_32xff()
  assert(addr >= BASE && addr + size <= BASE + META_SIZE);
  for (unsigned i = 0; i < size; i++)
    if (flashmem[addr - BASE + i] != 0xFF)
      return false;
  return true;
}

bool flash_erase_sector(uintptr_t addr) {
  assert(addr >= BASE && addr + 4096 <= BASE + META_SIZE && !(addr % 4096));
  memset(&flashmem[addr - BASE], 0xFF, 4096);
  return true;
}

bool flash_program(uint32_t addr, const uint8_t *buf, unsigned size) {
  assert(addr >= BASE);
  if (addr + size > max_written)
    max_written = addr + size;
  assert(addr + size <= BASE + META_SIZE);     // Never past the log
  for (unsigned i = 0; i < size; i++)
    flashmem[addr - BASE + i] &= buf[i];       // Bits only go from 1 to 0
  return true;
}

bool flash_verify(uint32_t addr, const uint8_t *buf, unsigned size) {
  return !memcmp(&flashmem[addr - BASE], buf, size);
}

static union {
  t_reg_entry e;
  uint8_t buf[sizeof(t_reg_entry) + FLASHG_MAXFN_CNT * sizeof(t_flash_game_entry)];
} tbl, got;

// A table of n games (game i uses block i + 1, named after tag).
static void make_table(unsigned n, char tag) {
  memset(&tbl, 0, sizeof(tbl));
  tbl.e.gamecnt = n;
  for (unsigned i = 0; i < n; i++) {
    tbl.e.games[i].gamecode = 0x41414141 + i;
    tbl.e.games[i].numblks = 1;
    tbl.e.games[i].blkmap[0] = i + 1;
    snprintf(tbl.e.games[i].game_name, sizeof(tbl.e.games[i].game_name), "/GBA/%c%u.gba", tag, i);
  }
}

static void expect_table(unsigned n, char tag) {
  assert(flashmgr_load(BASE, META_SIZE, &got.e) == 1);
  assert(got.e.gamecnt == n);
  for (unsigned i = 0; i < n; i++) {
    char name[32];
    snprintf(name, sizeof(name), "/GBA/%c%u.gba", tag, i);
    assert(!strcmp(got.e.games[i].game_name, name));
  }
}

// Where the next entry goes: after the last header with the magic.
static unsigned log_end() {
  unsigned off = 0;
  while (off + sizeof(t_reg_entry) <= META_SIZE) {
    const t_reg_entry *h = (t_reg_entry*)&flashmem[off];
    if (h->magic != NOR_ENTRY_MAGIC || h->gamecnt > FLASHG_MAXFN_CNT)
      break;
    off += sizeof(t_reg_entry) + h->gamecnt * sizeof(t_flash_game_entry);
  }
  return off;
}

int main() {
  memset(flashmem, 0xFF, sizeof(flashmem));

  // Nothing stored.
  assert(flashmgr_load(BASE, META_SIZE, &got.e) == 0);

  // Stored tables come back, the newest.
  make_table(2, 'A');
  assert(flashmgr_store(BASE, META_SIZE, &tbl.e));
  expect_table(2, 'A');
  make_table(3, 'B');
  assert(flashmgr_store(BASE, META_SIZE, &tbl.e));
  expect_table(3, 'B');

  // A store cut short (its games not written, so its checksum is wrong):
  // the table before it is loaded.
  unsigned end = log_end();
  t_reg_entry cut = { .magic = NOR_ENTRY_MAGIC, .crc = 0x12345678, .gamecnt = 1 };
  memcpy(&flashmem[end], &cut, sizeof(cut));
  expect_table(3, 'B');

  // The next store goes after it (that space is erased), and is loaded.
  make_table(1, 'C');
  assert(flashmgr_store(BASE, META_SIZE, &tbl.e));
  expect_table(1, 'C');

  // A store cut before its game count (all ones): the log ends there, and
  // the space isn't erased: the next store wipes the log, writes at 0.
  end = log_end();
  t_reg_entry cut2 = { .magic = NOR_ENTRY_MAGIC, .crc = 0, .gamecnt = 0xFFFFFFFF };
  memcpy(&flashmem[end], &cut2, sizeof(cut2));
  expect_table(1, 'C');
  make_table(2, 'D');
  assert(flashmgr_store(BASE, META_SIZE, &tbl.e));
  expect_table(2, 'D');
  assert(((t_reg_entry*)flashmem)->gamecnt == 2);      // At the start

  // Filling the log: stores never write past it (it's wiped when full).
  for (unsigned i = 0; i < 40; i++) {
    make_table(1 + i % 5, 'E' + i % 10);
    assert(flashmgr_store(BASE, META_SIZE, &tbl.e));
    expect_table(1 + i % 5, 'E' + i % 10);
  }
  assert(max_written <= BASE + META_SIZE);

  // Tables with a block that doesn't exist, or a name that doesn't end,
  // aren't valid.
  make_table(2, 'F');
  assert(flashmgr_check(&tbl.e) == false);              // No crc yet
  assert(flashmgr_store(BASE, META_SIZE, &tbl.e));
  assert(flashmgr_check(&tbl.e));
  tbl.e.games[1].blkmap[0] = 200;
  assert(!flashmgr_check(&tbl.e));
  make_table(2, 'G');
  assert(flashmgr_store(BASE, META_SIZE, &tbl.e));
  memset(tbl.e.games[0].game_name, 'x', sizeof(tbl.e.games[0].game_name));
  assert(!flashmgr_check(&tbl.e));

  printf("NOR table tests OK\n");
  return 0;
}
