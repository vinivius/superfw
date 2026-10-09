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

#include <string.h>
#include <stdlib.h>

#include "settings.h"
#include "fatfs/ff.h"
#include "fileutil.h"
#include "common.h"
#include "nanoprintf.h"
#include "util.h"

#pragma GCC optimize ("Os")

unsigned lang_lookup(uint16_t code);
uint16_t lang_getcode();

const t_combo_key hotkey_list[] = {
  {"L+R+Start",     0x00F7},
  {"L+R+Select",    0x00FB},
  {"L+R+Start+Sel", 0x00F3},
  {"L+R",           0x00FF},
  {"L+R+A",         0x00FE},
  {"L+R+B",         0x00FD},
  {"L+R+⯇+A",       0x00DE},
  {"L+R+⯈+B",       0x00ED},
  {"L+R+⯅+A",       0x00BE},
  {"L+R+⯆+A",       0x007E},
  {"A+B+Start",     0x03F4},
  {"A+B+Select",    0x03F8},
  {"A+B+Start+Sel", 0x03F0},
};

const char *save_paths[] = {
  "/SAVEGAME/",
  "/SAVES/",
};

const char *savestates_paths[] = {
  "/SAVESTATE/",
  "/.superfw/savestate/",
};

const char *savestates_paths_display[] = {
  "/SAVESTATE/",
  ".sfw/savestate",
};

const uint8_t animspd_lut[] = {
  2,    //  8 pix/second
  3,    // 12 pix/second
  6,    // 24 pix/second
  8,    // 32 pix/second
  12,   // 48 pix/second
};

// Menu settings
uint8_t menu_theme = 0;
uint8_t lang_id = 0;
uint8_t recent_menu = 1;
uint8_t hide_hidden = 0;
uint8_t anim_speed = animspd_cnt / 2;
uint8_t boxart_enabled = 1;
uint8_t hide_ext = 1;          // Hide the extension of ROM files

// Default settings
t_patch_policy patcher_default = PatchAuto;

uint8_t boot_bios_splash = 0;   // Whether the BIOS boots to the splash screen
uint8_t use_slowld = 0;         // Use slow mirrors for ROM loading, check loaded data.
uint8_t use_fastew = 0;         // Overclock EWRAM while playing.
uint8_t use_verify_nor = 0;     // Verify flash writes
uint8_t use_verify_rom = 0;     // Re-read loaded ROMs from SD to verify them

uint8_t save_path_default = SaveSavegameDir;
uint8_t save_path_nor_default = SaveSavegameDir;
uint8_t state_path_default = StateSavestateDir;

uint8_t backup_sram_default = 0;  // Number of older SRAM save to keep as backup

uint8_t hotkey_combo = 0;  // Hotkey Combo number
uint8_t enable_cheats = 0; // By default cheats are disabled (it's slightly faster)

uint8_t autoload_default = 1;
uint8_t autosave_default = 1;
uint8_t autosave_prefer_ds = 1;
uint8_t ingamemenu_default = 1;
uint8_t rtcpatch_default = 1;
uint8_t rtcspeed_default = 3;

uint32_t rtcvalue_default = 45568800U;

// Setting loading/saving routines

// The settings files that couldn't be read (SD card errors; bit 0: settings,
// bit 1: UI settings): they aren't written over with the defaults (a reboot
// reads them again).
static unsigned settings_unread;

// Writes a settings file (in SUPERFW_DIR) whole, unless it couldn't be read.
static bool settings_write(const char *fn, unsigned unread_bit, const char *buf) {
  return !(settings_unread & unread_bit) && superfw_file_write(NULL, fn, buf, strlen(buf));
}

bool save_ui_settings() {
  // Serialize the settings
  uint16_t lc = lang_getcode();
  char buf[512];
  npf_snprintf(buf, sizeof(buf),
    "theme=%u\n"
    "langcode=%c%c\n"
    "recent_menu=%u\n"
    "anim_speed=%u\n"
    "hide_hidden=%u\n"
    "boxart=%u\n"
    "hide_ext=%u\n",
    menu_theme, (lc & 0xFF), (lc >> 8), recent_menu, anim_speed, hide_hidden, boxart_enabled, hide_ext);

  return settings_write(UISETTINGS_FILEPATH, 2, buf);
}

bool save_settings() {
  // Serialize the settings
  char buf[512];
  npf_snprintf(buf, sizeof(buf),
    "hotkey_opt=%u\n"
    "boot_to_bios=%u\n"
    "save_path_policy=%u\n"
    "save_path_nor_policy=%u\n"
    "state_path_policy=%u\n"
    "sram_backup_count=%u\n"
    "enable_cheats=%u\n"
    "enable_slowld=%u\n"
    "enable_fastewram=%u\n"
    "enable_norwrcheck=%u\n"
    "enable_romverify=%u\n"
    "default_patcher=%u\n"
    "default_igmenu=%u\n"
    "default_rtcpatch=%u\n"
    "default_rtctick=%u\n"
    "default_loadgame=%u\n"
    "default_savegame=%u\n"
    "prefer_directsave=%u\n"
    "default_rtcts=%lu\n",
    hotkey_combo, boot_bios_splash, save_path_default, save_path_nor_default,
    state_path_default, backup_sram_default, enable_cheats, use_slowld, use_fastew,
    use_verify_nor, use_verify_rom, (unsigned int)patcher_default, ingamemenu_default, rtcpatch_default,
    rtcspeed_default, autoload_default, autosave_default, autosave_prefer_ds,
    rtcvalue_default);

  return settings_write(SETTINGS_FILEPATH, 1, buf);
}

static void parse_settings(void *usr, const char *var, const char *value) {
  unsigned valu = parseuint(value);
  if (!strcmp(var, "default_rtcts"))
    rtcvalue_default = valu;
  else {
    static const struct {
      const char *s;
      uint8_t * const var;
    } bolset[] = {
      { "boot_to_bios",      &boot_bios_splash },
      { "enable_cheats",     &enable_cheats },
      { "default_igmenu",    &ingamemenu_default },
      { "enable_slowld",     &use_slowld },
      { "enable_fastewram",  &use_fastew },
      { "enable_norwrcheck", &use_verify_nor },
      { "enable_romverify",  &use_verify_rom },
      { "default_rtcpatch",  &rtcpatch_default },
      { "default_loadgame",  &autoload_default },
      { "default_savegame",  &autosave_default },
      { "prefer_directsave", &autosave_prefer_ds },
    };
    for (unsigned i = 0; i < sizeof(bolset)/sizeof(bolset[0]); i++)
      if (!strcmp(var, bolset[i].s)) {
        *bolset[i].var = valu & 1;
        break;
      }

    static const struct {
      const char *s;
      uint8_t * const var;
      const unsigned modval;
    } uintset[] = {
      { "hotkey_opt",           &hotkey_combo,          sizeof(hotkey_list)/sizeof(hotkey_list[0]) },
      { "save_path_policy",     &save_path_default,     SaveDirCNT },
      { "save_path_nor_policy", &save_path_nor_default, SaveDirNORCNT },
      { "state_path_policy",    &state_path_default,    StateDirCNT },
      { "sram_backup_count",    &backup_sram_default,   MAX_BACKUP_CNT + 1 },
      { "default_patcher",      &patcher_default,       PatchTotalCNT },
      { "default_rtctick",      &rtcspeed_default,      RTC_SPEED_CNT },
    };
    for (unsigned i = 0; i < sizeof(uintset)/sizeof(uintset[0]); i++)
      if (!strcmp(var, uintset[i].s)) {
        *uintset[i].var = valu % uintset[i].modval;
        break;
      }
  }
}

static void parse_ui_settings(void *usr, const char *var, const char *value) {
  if (!strcmp(var, "langcode")) {
    uint16_t code = ((uint8_t)value[0]) | (((uint8_t)value[1]) << 8);
    lang_id = lang_lookup(code);
  } else {
    // Values index tables, keep them in range (the file could be edited).
    static const struct {
      const char *s;
      uint8_t * const var;
      const unsigned modval;
    } uintset[] = {
      { "theme",       &menu_theme,     MENU_THEME_COUNT },
      { "recent_menu", &recent_menu,    2 },
      { "hide_hidden", &hide_hidden,    2 },
      { "anim_speed",  &anim_speed,     animspd_cnt },
      { "boxart",      &boxart_enabled, 2 },
      { "hide_ext",    &hide_ext,       2 },
    };
    unsigned valu = parseuint(value);
    for (unsigned i = 0; i < sizeof(uintset)/sizeof(uintset[0]); i++)
      if (!strcmp(var, uintset[i].s)) {
        *uintset[i].var = valu % uintset[i].modval;
        break;
      }
  }
}

// Parses a config file and calls the user callback with varname+value
// Pointers are only valid for the duration of the callback!
static void parse_file(char *buf, void(*parse_cb)(void *usr, const char*, const char*), void *usrptr) {
  char *p = buf;
  while (1) {
    char *e = strchr(p, '\n');
    if (e)
      *e = 0;

    char *a = strchr(p, '=');
    if (a) {
      *a = 0;
      parse_cb(usrptr, p, &a[1]);
      *a = '=';
    }

    if (!e)
      break;

    *e = '\n';
    p = &e[1];  // Advance to the next line
  }
}

typedef void (*setting_fn)(void *usr, const char *var, const char *value);

static bool parse_setting_line(char *line, unsigned len, void *usr) {
  if (line)
    parse_file(line, *(setting_fn*)usr, NULL);
  return true;
}

// Settings files are read line by line, however long (hand edited) they are.
// Lines too long for a setting are skipped. False on SD card errors.
static bool load_settings_file(const char *fn, setting_fn parse_cb) {
  char buf[64];
  FRESULT res = read_lines_file(fn, buf, sizeof(buf), parse_setting_line, &parse_cb);
  return FR_OK == res || fr_missing(res);
}

void load_settings() {
  settings_unread = (load_settings_file(SETTINGS_FILEPATH, parse_settings) ? 0 : 1) |
                    (load_settings_file(UISETTINGS_FILEPATH, parse_ui_settings) ? 0 : 2);
}

void sram_filename_calc(const char *rom, char *savefn, unsigned save_path) {
  // Next to the ROM, or in a saves folder (also when the ROM's folder leaves
  // no room for its name).
  const unsigned maxlen = MAX_FN_LEN - 1 - SAVE_FN_RESERVE;
  if (save_path != SaveRomName || !derived_fn(savefn, maxlen, NULL, rom, ".sav"))
    derived_fn(savefn, maxlen, save_paths[save_path == SaveRomName ? SaveSavegameDir : save_path], rom, ".sav");
}

void savestate_filename_calc(const char *rom, char *statefn) {
  derived_fn(statefn, MAX_FN_LEN - 1 - STATE_FN_RESERVE, savestates_paths[state_path_default], rom, "");
}

static void parse_rom_load_settings(void *usr, const char *var, const char *value) {
  t_rom_load_settings *rs = (t_rom_load_settings*)usr;
  unsigned valu = parseuint(value);
  if (!strcmp(var, "rtc"))
    rs->use_rtc = valu & 1;
  else if (!strcmp(var, "igm"))
    rs->use_igm = valu & 1;
  else if (!strcmp(var, "directsaving"))
    rs->use_dsaving = valu & 1;
  else if (!strcmp(var, "patchmode"))
    rs->patch_policy = valu % PatchOptCNT;
}

static void parse_rom_launch_settings(void *usr, const char *var, const char *value) {
  t_rom_launch_settings *rs = (t_rom_launch_settings*)usr;
  unsigned valu = parseuint(value);
  if (!strcmp(var, "cheats"))
    rs->use_cheats = valu & 1;
  else if (!strcmp(var, "rtcts"))
    rs->rtcts = valu;
}

// The config file of a ROM: ROMCONFIG_PATH + its name + .config, the name
// shortened if needed to the FAT limit (FF_MAX_LFN).
#define ROM_CONFIG_FN_SIZE   (sizeof(ROMCONFIG_PATH) + FF_MAX_LFN)
static void rom_config_fn(char *cfgfn, const char *romfn) {
  derived_fn(cfgfn, ROM_CONFIG_FN_SIZE - 1, ROMCONFIG_PATH, romfn, ".config");
}


typedef struct {
  t_rom_load_settings *rld;
  t_rom_launch_settings *rlh;
} t_rom_settings;

static bool parse_rom_settings_line(char *line, unsigned len, void *usr) {
  const t_rom_settings *rs = (t_rom_settings*)usr;
  if (line && rs->rld)
    parse_file(line, parse_rom_load_settings, rs->rld);
  if (line && rs->rlh)
    parse_file(line, parse_rom_launch_settings, rs->rlh);
  return true;
}

// False on SD card errors (without a config, the defaults are kept).
bool load_rom_settings(const char *fn, t_rom_load_settings *rld, t_rom_launch_settings *rlh) {
  // Line by line, however long the (hand edited) file is: lines appended by
  // save_rom_patchmode() come last. Lines too long for a setting are skipped.
  char cfgfn[ROM_CONFIG_FN_SIZE], buf[64];
  rom_config_fn(cfgfn, fn);
  t_rom_settings rs = { rld, rlh };
  FRESULT res = read_lines_file(cfgfn, buf, sizeof(buf), parse_rom_settings_line, &rs);
  return FR_OK == res || fr_missing(res);
}

// Records the patch mode for a ROM, appended to its config (created if
// needed): later lines win, and the other settings keep following the
// global defaults unless the config already sets them.
bool save_rom_patchmode(const char *fn, unsigned mode) {
  char cfgfn[ROM_CONFIG_FN_SIZE];
  rom_config_fn(cfgfn, fn);
  FIL fd;
  if (!superfw_file_open(&fd, ROMCONFIG_PATH, cfgfn, FA_OPEN_APPEND))
    return false;
  // On a line of its own (a hand-edited file may not end in a newline).
  char buf[32];
  npf_snprintf(buf, sizeof(buf), "\npatchmode=%u\n", mode);
  return write_close(&fd, buf, strlen(buf));
}

bool save_rom_settings(const char *fn, const t_rom_load_settings *rld, const t_rom_launch_settings *rlh) {
  // Serialize the ROM settings
  char buf[128];
  npf_snprintf(buf, sizeof(buf),
    "patchmode=%u\n"
    "igm=%u\n"
    "rtc=%u\n"
    "directsaving=%u\n"
    "cheats=%u\n"
    "rtcts=%u\n",
    rld->patch_policy,
    rld->use_igm ? 1 : 0,
    rld->use_rtc ? 1 : 0,
    rld->use_dsaving ? 1 : 0,
    rlh->use_cheats ? 1 : 0,
    (unsigned int)rlh->rtcts);

  char cfgfn[ROM_CONFIG_FN_SIZE];
  rom_config_fn(cfgfn, fn);
  return superfw_file_write(ROMCONFIG_PATH, cfgfn, buf, strlen(buf));
}


