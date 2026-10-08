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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.   See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "util.h"

#include "fatfs/ff.h"
#include "fileutil.h"

unsigned mkdir_cnt = 0;
const char *expected_mkdirs[] = {
  "/path",
  "/foo",
  "/foo/bar",
  "/foo/bar/lol",
};
FRESULT f_mkdir (const TCHAR* path) {
  assert(mkdir_cnt < sizeof(expected_mkdirs) / sizeof(expected_mkdirs[0]));
  assert(!strcmp(path, expected_mkdirs[mkdir_cnt++]));
  return FR_OK;
}
FRESULT f_stat (const TCHAR* path, FILINFO* fno) {
  return FR_OK;
}

// f_read() serves this text (all that's asked, as FatFs does, unless at its end).
static const char *rd_text;
static unsigned rd_off;
FRESULT f_read (FIL* fp, void* buff, UINT btr, UINT* br) {
  unsigned n = strlen(&rd_text[rd_off]);
  *br = n < btr ? n : btr;
  memcpy(buff, &rd_text[rd_off], *br);
  rd_off += *br;
  return FR_OK;
}

// f_write() takes up to wr_room bytes, f_close() returns cl_res.
static unsigned wr_room;
static FRESULT cl_res;
FRESULT f_write (FIL* fp, const void* buff, UINT btw, UINT* bw) {
  *bw = btw < wr_room ? btw : wr_room;
  return FR_OK;
}
FRESULT f_close (FIL* fp) {
  return cl_res;
}

static char rd_lines[256];
static unsigned rd_count;
static bool collect_line(char *line, unsigned len, void *usr) {
  assert(strlen(line) == len);
  strcat(rd_lines, line);
  strcat(rd_lines, "|");
  return ++rd_count < *(unsigned*)usr;
}

static const char *read_all(const char *text, unsigned bufsize, unsigned maxlines) {
  char buf[64];
  rd_text = text;
  rd_off = rd_count = 0;
  rd_lines[0] = 0;
  assert(read_lines(NULL, buf, bufsize, collect_line, &maxlines));
  return rd_lines;
}

int main() {
  char tmp[1024];

  // Lines (CRLF too) without their newline, the last one with or without
  // one; lines that don't fit the buffer (9 bytes, a newline included) are
  // skipped whole; the callback can stop it.
  assert(!strcmp(read_all("a=1\nbb=2\r\n\nlast", 10, 99), "a=1|bb=2||last|"));
  assert(!strcmp(read_all("12345678\n123456789\nx\n1234567890123456789012\ny", 10, 99), "12345678|x|y|"));
  assert(!strcmp(read_all("toolongforit", 10, 99), ""));
  assert(!strcmp(read_all("a\nb\nc\n", 10, 2), "a|b|"));
  assert(!strcmp(read_all("", 10, 99), ""));

  // write_close(): the data must be all written and the file closed.
  wr_room = 100; cl_res = FR_OK;
  assert(write_close(NULL, "abc", 3));
  cl_res = FR_DISK_ERR;
  assert(!write_close(NULL, "abc", 3));
  wr_room = 2; cl_res = FR_OK;
  assert(!write_close(NULL, "abc", 3));

  assert(0 == parseuint("0"));
  assert(1 == parseuint("1"));
  assert(123 == parseuint("123"));
  assert(4294967295 == parseuint("4294967295"));

  assert(!strcmp("", file_basename("")));
  assert(!strcmp("foo", file_basename("/foo")));
  assert(!strcmp("foo", file_basename("foo")));
  assert(!strcmp("test", file_basename("/foo/bar/lol/test")));

  assert(check_file_exists("/test"));
  assert(check_file_exists("/test/lol"));

  create_basepath(NULL);
  create_basepath("");
  create_basepath("/");
  create_basepath("/justafile");
  create_basepath("/path/justafile");
  create_basepath("/foo/bar/lol/test");

  file_dirname("/test/path1/path2/file", tmp);
  assert(!strcmp(tmp, "/test/path1/path2"));
  file_dirname("/", tmp);
  assert(!strcmp(tmp, ""));
  file_dirname("/file", tmp);
  assert(!strcmp(tmp, ""));

  strcpy(tmp, "/foo/bar/lol.txt");
  replace_extension(tmp, ".pdf");
  assert(!strcmp(tmp, "/foo/bar/lol.pdf"));

  strcpy(tmp, "/foo/bar/lol.txt");
  replace_extension(tmp, "");
  assert(!strcmp(tmp, "/foo/bar/lol"));

  strcpy(tmp, "/foo/bar/lol");
  replace_extension(tmp, ".doc");
  assert(!strcmp(tmp, "/foo/bar/lol.doc"));

  assert(!strcmp(find_extension("/foo/bar.lol"), "lol"));
  assert(find_extension("/foo/barlol") == NULL);
  assert(find_extension("/barlol") == NULL);
  assert(find_extension("foo") == NULL);
  assert(!strcmp(find_extension("/foo/bar."), ""));
  assert(!strcmp(find_extension("/foo/bar.lol/test.123"), "123"));
  assert(find_extension("/foo/bar.lol/beef") == NULL);

  // Paths derived from a ROM name (ie. saves, configs).
  assert(derived_fn(tmp, 255, "/.superfw/config/", "/GBA/Game.gba", ".config"));
  assert(!strcmp(tmp, "/.superfw/config/Game.config"));
  assert(derived_fn(tmp, 255, NULL, "/GBA/Game.gba", ".sav"));
  assert(!strcmp(tmp, "/GBA/Game.sav"));
  assert(derived_fn(tmp, 255, NULL, "/GBA/Game", ".sav"));
  assert(!strcmp(tmp, "/GBA/Game.sav"));
  assert(derived_fn(tmp, 255, "/SAVESTATE/", "Game.v1.gba", ""));
  assert(!strcmp(tmp, "/SAVESTATE/Game.v1"));
  assert(derived_fn(tmp, 255, NULL, "/GBA/.hidden", ".sav"));     // As replace_extension()
  assert(!strcmp(tmp, "/GBA/.sav"));
  // Names that don't fit are cut short (never in the middle of a UTF-8
  // character) and end in "~" and a hash of the whole name, so they differ.
  assert(derived_fn(tmp, 26, "/SAVES/", "/x/ABCDEFGHIJKLMNOPQRSTUVWXYZ.gba", ".sav"));
  assert(strlen(tmp) == 26 && !memcmp(tmp, "/SAVES/ABCDEF~", 14) && !strcmp(&tmp[22], ".sav"));
  {
    // Other names differ; names differing only in case hash the same (FAT
    // compares names that way).
    char other[64];
    assert(derived_fn(other, 26, "/SAVES/", "/x/ABCDEFGHIJKLMNOPQRSTUVWXYQ.gba", ".sav"));
    assert(strlen(other) == 26 && !memcmp(other, "/SAVES/ABCDEF~", 14) && strcmp(tmp, other));
    assert(derived_fn(other, 26, "/SAVES/", "/x/ABCDEFGHIJKLMNOPQRSTUVWXYz.gba", ".sav"));
    assert(!strcmp(tmp, other));
  }
  #define AE6 "a\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9.gba"    // 13 bytes before .gba
  assert(derived_fn(tmp, 12, "/", AE6, ""));
  assert(strlen(tmp) == 11 && !memcmp(tmp, "/a~", 3));
  assert(derived_fn(tmp, 13, "/", AE6, ""));
  assert(strlen(tmp) == 13 && !memcmp(tmp, "/a\xc3\xa9~", 5));
  // Not even the directory, the hash and the extension fit.
  assert(!derived_fn(tmp, 9, "/SAVES/", "/GBA/Game.gba", ".sav"));
  assert(!strcmp(tmp, ""));
  assert(!derived_fn(tmp, 19, "/SAVES/", "/GBA/LongerGame.gba", ".sav"));
  assert(derived_fn(tmp, 20, "/SAVES/", "/GBA/LongerGame.gba", ".sav"));
  assert(!memcmp(tmp, "/SAVES/~", 8) && strlen(tmp) == 20);
  {
    // The longest FAT name, into a buffer of exactly maxlen + 1 bytes.
    char name[257], *out = malloc(256);
    name[0] = '/';
    memset(&name[1], 'N', 255);
    name[256] = 0;
    assert(derived_fn(out, 255, "/.superfw/savestate/", name, ""));
    assert(strlen(out) == 255 && !memcmp(out, "/.superfw/savestate/NNN", 23) && out[246] == '~');
    free(out);
  }

  human_size(tmp, sizeof(tmp), 0); assert(!strcmp(tmp, "1K"));
  human_size(tmp, sizeof(tmp), 100); assert(!strcmp(tmp, "1K"));
  human_size(tmp, sizeof(tmp), 1000); assert(!strcmp(tmp, "1K"));
  human_size(tmp, sizeof(tmp), 1024); assert(!strcmp(tmp, "1K"));
  human_size(tmp, sizeof(tmp), 2047); assert(!strcmp(tmp, "1K"));
  human_size(tmp, sizeof(tmp), 2048); assert(!strcmp(tmp, "2K"));
  human_size(tmp, sizeof(tmp), 1023*1024); assert(!strcmp(tmp, "1023K"));
  human_size(tmp, sizeof(tmp), 1024*1024); assert(!strcmp(tmp, "1M"));

  human_size_kb(tmp, sizeof(tmp), 0); assert(!strcmp(tmp, "<1MiB"));
  human_size_kb(tmp, sizeof(tmp), 100); assert(!strcmp(tmp, "<1MiB"));
  human_size_kb(tmp, sizeof(tmp), 1024); assert(!strcmp(tmp, "1.0MiB"));
  human_size_kb(tmp, sizeof(tmp), 1025); assert(!strcmp(tmp, "1.0MiB"));
  human_size_kb(tmp, sizeof(tmp), 1125); assert(!strcmp(tmp, "1.0MiB"));
  human_size_kb(tmp, sizeof(tmp), 1127); assert(!strcmp(tmp, "1.1MiB"));
  human_size_kb(tmp, sizeof(tmp), 1023*1024); assert(!strcmp(tmp, "1023.0MiB"));
  human_size_kb(tmp, sizeof(tmp), 1024*1024); assert(!strcmp(tmp, "1.0GiB"));

  // 2000-01-01 00:00:00 (ts: 946684800)
  const t_dec_date d1 = {.year = 0, .month = 1, .day = 1, .hour = 0, .min = 0, .sec = 0};
  assert(0 == date2timestamp(&d1));
  const t_dec_date d2 = {.year = 34, .month = 7, .day = 21, .hour = 14, .min = 12, .sec = 3};
  assert(1090419123 == date2timestamp(&d2));
  const t_dec_date d3 = {.year = 55, .month = 12, .day = 31, .hour = 23, .min = 59, .sec = 59};
  assert(1767225599 == date2timestamp(&d3));
  for (unsigned i = 0; i < 98; i++) {
    t_dec_date d = {.year = i, .month = 1, .day = 1, .hour = 0, .min = 0, .sec = 0};
    assert(86400 * ((i + 3) / 4) + 31536000 * i == date2timestamp(&d));
  }

  t_dec_date o;
  timestamp2date(0, &o);
  assert(!memcmp(&o, &d1, sizeof(d1)));
  timestamp2date(1090419123, &o);
  assert(!memcmp(&o, &d2, sizeof(d2)));
  timestamp2date(1767225599, &o);
  assert(!memcmp(&o, &d3, sizeof(d3)));

  // fixdate
  t_dec_date f1 = {.year = 100, .month = 1, .day = 1, .hour = 0, .min = 0, .sec = 0};
  fixdate(&f1); assert(f1.year == 0);
  t_dec_date f2 = {.year = -1, .month = 1, .day = 1, .hour = 0, .min = 0, .sec = 0};
  fixdate(&f2); assert(f2.year == 99);

  t_dec_date f3 = {.year = 0, .month = 1, .day = 1, .hour = 24, .min = 0, .sec = 0};
  fixdate(&f3); assert(f3.hour == 0);
  t_dec_date f4 = {.year = 0, .month = 1, .day = 1, .hour = -1, .min = 0, .sec = 0};
  fixdate(&f4); assert(f4.hour == 23);

  t_dec_date f5 = {.year = 0, .month = 1, .day = 1, .hour = 0, .min = 60, .sec = 0};
  fixdate(&f5); assert(f5.min == 0);
  t_dec_date f6 = {.year = 0, .month = 1, .day = 1, .hour = 0, .min = -1, .sec = 0};
  fixdate(&f6); assert(f6.min == 59);

  t_dec_date f7 = {.year = 0, .month = 1, .day = 1, .hour = 0, .min = 0, .sec = 60};
  fixdate(&f7); assert(f7.sec == 0);
  t_dec_date f8 = {.year = 0, .month = 1, .day = 1, .hour = 0, .min = 0, .sec = -1};
  fixdate(&f8); assert(f8.sec == 59);

  t_dec_date f9 = {.year = 0, .month = 0, .day = 1, .hour = 0, .min = 0, .sec = 0};
  fixdate(&f9); assert(f9.month == 12);
  t_dec_date f10 = {.year = 0, .month = 13, .day = 1, .hour = 0, .min = 0, .sec = 0};
  fixdate(&f10); assert(f10.month == 1);

  // day clamp, leap-year aware (year 0 leap, year 1 not)
  t_dec_date f11 = {.year = 0, .month = 2, .day = 30, .hour = 0, .min = 0, .sec = 0};
  fixdate(&f11); assert(f11.day == 1);
  t_dec_date f12 = {.year = 0, .month = 2, .day = 0, .hour = 0, .min = 0, .sec = 0};
  fixdate(&f12); assert(f12.day == 29);
  t_dec_date f13 = {.year = 1, .month = 2, .day = 0, .hour = 0, .min = 0, .sec = 0};
  fixdate(&f13); assert(f13.day == 28);
  t_dec_date f14 = {.year = 0, .month = 4, .day = 31, .hour = 0, .min = 0, .sec = 0};
  fixdate(&f14); assert(f14.day == 1);

  // memcpy32 / memset32 / memmove32
  // NB: buffers use uint32_t arrays to keep them 4-byte aligned, as required
  // by the forced 32-bit accesses in these functions (embedded, no byte loop).

  uint32_t mc_src[4] = {1, 2, 3, 4};
  uint32_t mc_dst[4] = {0, 0, 0, 0};
  memcpy32(mc_dst, mc_src, 16);
  assert(!memcmp(mc_dst, mc_src, 16));

  uint32_t mc_dst2[4] = {0xaa, 0xaa, 0xaa, 0xaa};
  memcpy32(mc_dst2, mc_src, 8);  // partial: only first 2 words
  assert(mc_dst2[0] == 1 && mc_dst2[1] == 2);
  assert(mc_dst2[2] == 0xaa && mc_dst2[3] == 0xaa);

  uint32_t ms_dst[4] = {0, 0, 0, 0};
  memset32(ms_dst, 0xdeadbeef, 16);
  assert(ms_dst[0] == 0xdeadbeef && ms_dst[1] == 0xdeadbeef &&
         ms_dst[2] == 0xdeadbeef && ms_dst[3] == 0xdeadbeef);

  uint32_t ms_dst2[4] = {0, 0, 0, 0};
  memset32(ms_dst2, 0x11111111, 8);  // partial: only first 2 words
  assert(ms_dst2[0] == 0x11111111 && ms_dst2[1] == 0x11111111);
  assert(ms_dst2[2] == 0 && ms_dst2[3] == 0);

  // memmove32 forward copy (dst < src, overlapping)
  uint32_t mv1[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  memmove32(&mv1[0], &mv1[2], 24);  // shift left by 2 words
  uint32_t mv1_exp[8] = {3, 4, 5, 6, 7, 8, 7, 8};
  assert(!memcmp(mv1, mv1_exp, sizeof(mv1)));

  // memmove32 backward copy (dst > src, overlapping)
  uint32_t mv2[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  memmove32(&mv2[2], &mv2[0], 24);  // shift right by 2 words
  uint32_t mv2_exp[8] = {1, 2, 1, 2, 3, 4, 5, 6};
  assert(!memcmp(mv2, mv2_exp, sizeof(mv2)));

  // memmove32 same pointer: no-op
  uint32_t mv3[4] = {1, 2, 3, 4};
  memmove32(mv3, mv3, 16);
  uint32_t mv3_exp[4] = {1, 2, 3, 4};
  assert(!memcmp(mv3, mv3_exp, sizeof(mv3)));

  // memmove32 non-overlapping
  uint32_t mv4_src[4] = {9, 8, 7, 6};
  uint32_t mv4_dst[4] = {0, 0, 0, 0};
  memmove32(mv4_dst, mv4_src, 16);
  assert(!memcmp(mv4_dst, mv4_src, 16));

  // memmove32 with non-word-multiple count: truncated down to nearest 4
  uint32_t mv5[4] = {1, 2, 3, 4};
  memmove32(&mv5[1], &mv5[0], 15);  // count -> 12, i.e. 3 words
  uint32_t mv5_exp[4] = {1, 1, 2, 3};
  assert(!memcmp(mv5, mv5_exp, sizeof(mv5)));
}


