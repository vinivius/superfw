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

// Scans a ROM in chunks of chunk bytes, each with PE_LOOKBACK bytes before it
// and PE_LOOKAHEAD after it (zeros past the ROM's end), as the firmware does.
static void scan(const uint8_t *rom, unsigned size, unsigned chunk, t_patch_builder *pb) {
  uint8_t *buf = malloc(PE_LOOKBACK + chunk + PE_LOOKAHEAD);
  patchengine_init(pb, size);
  for (unsigned i = 0; i < size; i += chunk) {
    const unsigned start = i < PE_LOOKBACK ? 0 : i - PE_LOOKBACK;
    const unsigned blksize = i + chunk <= size ? chunk : size - i;
    const unsigned end = i + blksize + PE_LOOKAHEAD;
    memset(buf, 0, PE_LOOKBACK + chunk + PE_LOOKAHEAD);
    memcpy(buf, &rom[start], (end < size ? end : size) - start);
    patchengine_process_rom((uint32_t*)buf, (i - start) / 4, (blksize + 3) / 4, start, pb, noprogress);
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
