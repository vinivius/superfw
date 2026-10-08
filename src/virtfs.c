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
#include <string.h>

#include "common.h"

const t_vfile *get_vfile(const char *fname) {
  const t_vfile *f = (const t_vfile*)ROM_ASSETS_U8;
  while (f->size) {
    if (!memcmp(f->fn, fname, sizeof(f->fn)))
      return f;
    f = (const t_vfile*)&f->payload[ROUND_UP2(f->size, 4)];
  }

  return NULL;
}

