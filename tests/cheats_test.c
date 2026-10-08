
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "fatfs/ff.h"
#include "cheats.h"

// Fake FatFS implementation
FRESULT f_open (FIL* fp, const TCHAR* path, BYTE mode) {
  FILE *fd = fopen(path, "rb");
  if (!fd)
    return FR_NO_FILE;

  *(FILE**)fp = fd;
  return FR_OK;
}

FRESULT f_read (FIL* fp, void* buff, UINT btr, UINT* br) {
  FILE *fd = *(FILE**)fp;
  size_t ret = fread(buff, 1, btr, fd);
  if (ret < 0)
    return FR_DISK_ERR;
  *br = ret;
  return FR_OK;
}

FRESULT f_close (FIL* fp) {
  FILE *fd = *(FILE**)fp;
  fclose(fd);
  return FR_OK;
}

const uint32_t cheat4_extra[] = { 0x0101000F, 0x00000002 };  // Code 4 (slide code)
const uint32_t cheat5_extra[] = { 0x78563412, 0x0000CDAB, 0x21436587, 0x00003412 };  // Code 5, copy buffer

const t_cheat_predec cheat1[] = {{ .opcode = 3, .blen = 8, .value = 0x0123, .address = 0x0300ABCD}};
const t_cheat_predec cheat2[] = {{ .opcode = 8, .blen = 8, .value = 0xABCD, .address = 0x03123456}};
const t_cheat_predec cheat3[] = {
  { .opcode = 3, .blen = 8, .value = 0x0010, .address = 0x0300AB01},
  { .opcode = 3, .blen = 8, .value = 0x0020, .address = 0x0300AB02},
  { .opcode = 3, .blen = 8, .value = 0x0030, .address = 0x0300AB03},
  { .opcode = 3, .blen = 8, .value = 0x0040, .address = 0x0300AB04},
};
const t_cheat_predec cheat4[] = {{ .opcode = 4, .blen = 16, .value = 0x0101, .address = 0x03002C2E}};
const t_cheat_predec cheat5[] = {{ .opcode = 5, .blen = 24, .value = 0x0006, .address = 0x03002C2E }};

const struct {
  const char *title;
  unsigned num_codes;
  const t_cheat_predec *codes;
  const uint32_t *extra;
  unsigned num_extra;
} expected [] = {
  { "First cheat title", 1, cheat1 },
  { "Second cheat", 1, cheat2 },
  { "Third cheat", 4, cheat3 },
  { "Some real char using a slide code", 2, cheat4, cheat4_extra, 2 },
  { "Buffer write cheat", 3, cheat5, cheat5_extra, 4 },
};

int main() {
  uint8_t tmp[32*1024];

  // A cheat whose codes can't be used is left out, the others load; a file
  // without any usable cheat (or empty) isn't used.
  assert(open_read_cheats(tmp, sizeof(tmp), "data/bad1.cht") > 0 && *(uint32_t*)tmp == 1);
  assert(open_read_cheats(tmp, sizeof(tmp), "data/bad2.cht") > 0 && *(uint32_t*)tmp == 1);
  assert(open_read_cheats(tmp, sizeof(tmp), "data/bad3.cht") < 0);
  assert(open_read_cheats(tmp, sizeof(tmp), "data/bad4.cht") < 0);
  assert(open_read_cheats(tmp, sizeof(tmp), "data/bad5.cht") < 0);    // Over MAX_CHEAT_CODES codes
  assert(open_read_cheats(tmp, sizeof(tmp), "data/bad6.cht") < 0);    // A super code's values missing
  assert(open_read_cheats(tmp, sizeof(tmp), "data/zero.cht") < 0);    // Counts of 0 (cheat_exec loops)
  assert(open_read_cheats(tmp, sizeof(tmp), "data/empty.cht") < 0);
  assert(open_read_cheats(tmp, sizeof(tmp), "data/mixed.cht") > 0 && *(uint32_t*)tmp == 1);

  // Titles and codes pair by what they are: a title without codes, or one too
  // long to read (and its codes), is left out, the cheats after it load.
  assert(open_read_cheats(tmp, sizeof(tmp), "data/pairing.cht") > 0 && *(uint32_t*)tmp == 1);
  assert(!strcmp((char*)((t_cheathdr*)&tmp[4])->data, "Second"));
  assert(open_read_cheats(tmp, sizeof(tmp), "data/longtitle.cht") > 0 && *(uint32_t*)tmp == 2);
  assert(!strcmp((char*)((t_cheathdr*)&tmp[4])->data, "Second"));
  // Tabs between and after codes.
  assert(open_read_cheats(tmp, sizeof(tmp), "data/tabs.cht") > 0 && *(uint32_t*)tmp == 1 &&
         ((t_cheathdr*)&tmp[4])->codelen == 3 * 8);

  // Windows line endings, tabs, no final newline.
  assert(open_read_cheats(tmp, sizeof(tmp), "data/crlf.cht") > 0 && *(uint32_t*)tmp == 2);
  assert(!strcmp((char*)((t_cheathdr*)&tmp[4])->data, "CRLF title"));

  // A super code: its value counts halfwords, 3 a line.
  {
    assert(open_read_cheats(tmp, sizeof(tmp), "data/super.cht") > 0 && *(uint32_t*)tmp == 1);
    const t_cheathdr *e = (t_cheathdr*)&tmp[4];
    const t_cheat_predec *pc = (t_cheat_predec*)&e->data[e->slen];
    assert(pc->opcode == 5 * 2 && pc->value == 3 && pc->blen == 16 && e->codelen == 24);
  }

  // The limits: MAX_CHEAT_CODES codes, titles cut to MAX_CHEAT_TITLE bytes
  // (at a character start: the 2 byte one that would be cut is left out).
  {
    assert(open_read_cheats(tmp, sizeof(tmp), "data/limits.cht") > 0 && *(uint32_t*)tmp == 2);
    const t_cheathdr *e = (t_cheathdr*)&tmp[4];
    assert(strlen((char*)e->data) == MAX_CHEAT_TITLE && e->slen == 252);
    assert(e->codelen == (MAX_CHEAT_CODES + 1) * 8);
    e = (t_cheathdr*)&e->data[e->slen + e->codelen];
    assert(strlen((char*)e->data) == 250 && e->codelen == 16);
  }

  int ret = open_read_cheats(tmp, sizeof(tmp), "data/test.cht");
  assert(ret >= 0);
  assert(*(uint32_t*)tmp == sizeof(expected)/sizeof(expected[0]));

  // Parse the output buffer, see that it matches what we expect
  int i = 4, n = 0;
  while (i < ret) {
    t_cheathdr *e = (t_cheathdr*)&tmp[i];
    assert(e->enabled == 0);
    assert(!strcmp(expected[n].title, (char*)e->data));
    assert(e->slen == ((strlen(expected[n].title) + 1 + 3) & ~3U));
    assert(e->codelen == (expected[n].num_codes + 1) * 8);

    // Validate the opcodes
    unsigned off = 0, xoff = 0;
    for (unsigned j = 0; j < expected[n].num_codes; j++) {
      t_cheat_predec *pc = (t_cheat_predec*)&e->data[e->slen + off];
      assert(pc->opcode == expected[n].codes[j].opcode * 2);
      assert(pc->blen == expected[n].codes[j].blen);
      assert(pc->value == expected[n].codes[j].value);
      assert(pc->address == expected[n].codes[j].address);

      // Validate the trailing payload words (byteswapped for opcode 5)
      unsigned nwords = (pc->blen - 8) / 4;
      const uint32_t *pw = (const uint32_t*)&e->data[e->slen + off + 8];
      assert(xoff + nwords <= expected[n].num_extra);
      for (unsigned k = 0; k < nwords; k++)
        assert(pw[k] == expected[n].extra[xoff + k]);
      xoff += nwords;

      off += pc->blen;
      j += (pc->blen - 8) / 8;
    }
    assert(xoff == expected[n].num_extra);   // no payload left unchecked

    // Code block ends with a single zero terminator entry
    assert(off + 8 == e->codelen);
    for (unsigned k = 0; k < 8; k++)
      assert(e->data[e->slen + off + k] == 0);
    n++;
    i += sizeof(t_cheathdr) + e->slen + e->codelen;
  }
  assert (n == sizeof(expected)/sizeof(expected[0]));
}

