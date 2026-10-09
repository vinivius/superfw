// Tests for the patch engine scan (patchengine.c) over a ROM in chunks, the
// way generate_patches_progress() (menu.c) feeds it.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "common.h"
#include "patchengine.h"

#define WAITCNT    0x04000204
#define IRQHADDR   0x03007FFC

static void noprogress(unsigned p) {}

// An ARM "LDR r0, [pc, #imm]" at word i loading word target.
static uint32_t arm_ldr(unsigned i, unsigned target) {
  return 0xE59F0000 | ((target - i - 2) * 4);
}

// Scans a ROM in chunks of chunk bytes as the firmware does (the bytes
// patchengine_chunk() says, zeros past the ROM's end), in a buffer of 8MiB.
static void scan(const uint8_t *rom, unsigned size, unsigned chunk, t_patch_builder *pb) {
  uint8_t *buf = malloc(8 * 1024 * 1024);
  patchengine_init(pb, size);
  for (unsigned i = 0; i < size; i += chunk) {
    t_pe_chunk c;
    patchengine_chunk(size, i, chunk, &c);
    assert(c.end - c.start <= 8 * 1024 * 1024);
    memset(buf, 0, 8 * 1024 * 1024);
    memcpy(buf, &rom[c.start], (c.end < size ? c.end : size) - c.start);
    patchengine_process_rom((uint32_t*)buf, c.first, c.count, c.start, pb, noprogress);
  }
  free(buf);
  patchengine_finalize(pb);
}

int main() {
  // A 12MiB ROM, scanned in 8MiB chunks: a WAITCNT constant at 9MiB (its LDR
  // before it) is patched at its ROM offset, not its chunk's.
  const unsigned size = 12 * 1024 * 1024, chunk = 8 * 1024 * 1024 - PE_LOOKBACK - PE_LOOKAHEAD;
  uint32_t *rom = calloc(1, size);
  const unsigned w = 9 * 1024 * 1024 / 4 + 2;
  rom[w] = WAITCNT;
  rom[w - 8] = arm_ldr(w - 8, w);
  // And one right after a chunk starts, its LDR in the chunk before.
  const unsigned w2 = chunk / 4 + 4;
  rom[w2] = WAITCNT;
  rom[w2 - 16] = arm_ldr(w2 - 16, w2);

  t_patch_builder pb;
  scan((uint8_t*)rom, size, chunk, &pb);
  assert(pb.p.wcnt_ops == 2 && !pb.overflow);
  assert((pb.p.op[0] & 0x1FFFFFF) == w2 * 4);
  assert((pb.p.op[1] & 0x1FFFFFF) == w * 4);

  // An ARM LDR at the farthest it reaches (4092 bytes past PC + 8) is found.
  memset(rom, 0, size);
  const unsigned w3 = 3 * 1024 * 1024 / 4;
  rom[w3] = WAITCNT;
  rom[w3 - 1025] = arm_ldr(w3 - 1025, w3);
  assert(rom[w3 - 1025] == (0xE59F0000 | 4092));
  scan((uint8_t*)rom, size, chunk, &pb);
  assert(pb.p.wcnt_ops == 1 && (pb.p.op[0] & 0x1FFFFFF) == w3 * 4);

  // More IRQ handler loads than a patch holds: the ops stop at MAX_PATCH_OPS
  // (nothing past the table, ASan checks it) and the builder knows.
  memset(rom, 0, size);
  for (unsigned i = 0; i < 200; i++) {
    const unsigned c = 4096 + i * 64;
    rom[c] = IRQHADDR;
    rom[c - 4] = arm_ldr(c - 4, c);
  }
  scan((uint8_t*)rom, size, chunk, &pb);
  assert(pb.overflow);
  assert(pb.p.wcnt_ops + pb.p.save_ops + pb.p.irqh_ops + pb.p.rtc_ops <= MAX_PATCH_OPS);

  free(rom);
  printf("Patch engine tests OK\n");
  return 0;
}
