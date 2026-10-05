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
#include <string.h>

#include "gbahw.h"
#include "settings.h"
#include "save.h"
#include "patchengine.h"
#include "supercard_driver.h"
#include "nanoprintf.h"
#include "fonts/font_render.h"
#include "common.h"
#include "fatfs/ff.h"
#include "flash.h"

// Global variables
FATFS sdfs;          // FatFS mounted filesystem
bool isgba = true;   // Has some alternative paths for NDS.
bool fastew = false; // EWRAM can be overclocked (from the look of it at least).
bool slowsd = true;  // Whether we use slow SD mirrors for SD operations.

t_flash_info flashinfo;
t_card_info sd_info;
t_patchdb_info pdbinfo;

void *font_base_addr = (void*)ROM_FONTBASE_U8;

static void wait_for_vblank() {
  while (!(REG_DISPSTAT & DISPSTAT_VBLANK));
}

void setup_video() {
  // Stop screen, clear VRAM and palette RAM.
  REG_DISPCNT = 0x80;
  dma_memset16(MEM_VRAM, 0xffff, MEM_VRAM_SIZE / 2);
  dma_memset16(MEM_PALETTE, 0xffff, MEM_PALETTE_SIZE / 2);
  MEM_PALETTE[0] = 0x0;

  // Setup BG mode 4, with single buffering, enable display now!
  wait_for_vblank();
  REG_DISPCNT = 0x4 | 0x1400 | 0x40;
}

#define display_info_msg_fmt(msg, ...) {   \
  unsigned off = (isgba ? (SCREEN_HEIGHT - 32) * SCREEN_WIDTH + 16 : \
                          (NDS_SCREEN_HEIGHT - 32) * NDS_SCREEN_WIDTH + 16); \
  uint8_t *basept = (uint8_t*)&MEM_VRAM_U8[off]; \
  char buf[40];                       \
  npf_snprintf(buf, sizeof(buf), msg, __VA_ARGS__); \
  draw_text_idx8_bus16(buf, basept, isgba ? SCREEN_WIDTH : NDS_SCREEN_WIDTH, 0x5); \
}

#define display_info_msg(msg) {   \
  unsigned off = (isgba ? (SCREEN_HEIGHT - 32) * SCREEN_WIDTH + 16 : \
                          (NDS_SCREEN_HEIGHT - 32) * NDS_SCREEN_WIDTH + 16); \
  uint8_t *basept = (uint8_t*)&MEM_VRAM_U8[off]; \
  draw_text_idx8_bus16(msg, basept, isgba ? SCREEN_WIDTH : NDS_SCREEN_WIDTH, 0x5); \
}

#define display_info_clear() {   \
  unsigned off = (isgba ? (SCREEN_HEIGHT - 32) * SCREEN_WIDTH + 16 : \
                          (NDS_SCREEN_HEIGHT - 32) * NDS_SCREEN_WIDTH + 16); \
  uint8_t *basept = (uint8_t*)&MEM_VRAM_U8[off]; \
  dma_memset16(basept, 0x0000, 8 * (isgba ? SCREEN_WIDTH : NDS_SCREEN_WIDTH)); \
}

#define fatal_init_error(msg, ...) {    \
  display_info_msg_fmt(msg, __VA_ARGS__);   \
  while(1);    /* Hang in here, not much to do! */ \
}

void init_sdcard_and_mount() {
  // Init the SD card hardware
  unsigned ret = sdcard_init(&sd_info);
  if (ret)
    fatal_init_error("Fatal SD card init err: %d", ret);

  // Attempt to mount the FAT/exFAT filesystem, see if it is valid!
  ret = f_mount(&sdfs, "0:", 1);
  if (ret)
    fatal_init_error("Cannot mount FATfs: %d", ret);
}

void check_pending_saves() {
  // Check if there's a pending save in the SRAM, and dump it.
  // TODO: add some key combo to skip this?
  if (FR_OK == f_stat(PENDING_SAVE_FILEPATH, NULL)) {
    display_info_msg("Writing previous savegame ...");

    unsigned ecode = flush_pending_sram();
    WRITE_LOG("Pending save flush result: %u", ecode);
    sdcard_flush_log();
    if (ecode == ERR_SAVE_FLUSH_WRITEFAIL) {
      // Display error messages briefly if any
      display_info_clear();
      display_info_msg("Failed to write savegame to SD!");
      wait_ms(4000);
      // Keep the sentinel file, so the save is retried on the next boot
      // (the SRAM still holds the game data as long as no game is loaded).
      return;
    }

    // Delete the sentinel file, the save was flushed (or is not recoverable).
    f_unlink(PENDING_SAVE_FILEPATH);
  }
}

volatile unsigned frame_count = 0;

#ifdef ENABLE_UART_LOGGING
// Serial debug control (UART debug builds only). Characters received over the
// link cable are polled on every V-blank, so they work even if the main code
// is stuck in a loop (as long as interrupts are enabled):
//   a b      A / B buttons        u d l r   D-pad
//   L R      L / R triggers       s e       Start / Select
//   [...]    press several keys at once, ie. "[LRu]"
//   !        reboot into SuperFW (BIOS hard reset)
//   P        send a screenshot of the menu
//   X        enter file transfer mode (see uart_xfer.c)
// Each key stays pressed until the menu has read it, then released until the
// menu has read that too, so no press is lost while the menu is busy.
volatile uint16_t uart_keys = 0;      // Currently injected (pressed) keys
volatile bool uart_keys_seen = false; // Set once the V-blank key sampling saw them
static uint16_t uart_q[32];
static unsigned uart_qh = 0, uart_qt = 0, uart_state = 0;
static uint16_t uart_combo = 0;
static bool uart_in_combo = false;

static uint16_t uart_key_for(uint8_t c) {
  switch (c) {
  case 'a': return KEY_BUTTA;     case 'b': return KEY_BUTTB;
  case 'u': return KEY_BUTTUP;    case 'd': return KEY_BUTTDOWN;
  case 'l': return KEY_BUTTLEFT;  case 'r': return KEY_BUTTRIGHT;
  case 'L': return KEY_BUTTL;     case 'R': return KEY_BUTTR;
  case 's': return KEY_BUTTSTA;   case 'e': return KEY_BUTTSEL;
  default:  return 0;
  };
}

static void uart_enqueue(uint16_t keys) {
  unsigned nt = (uart_qt + 1) % (sizeof(uart_q) / sizeof(uart_q[0]));
  if (keys && nt != uart_qh) {
    uart_q[uart_qt] = keys;
    uart_qt = nt;
  }
}

void uart_write(const void *data, unsigned size);

// Sends the displayed menu frame over the UART: Mode 4 frame + palettes,
// OAM and OBJ tiles (so icons can be drawn too). ~57KB, takes ~5 seconds.
//   "\n@@SCR1" DISPCNT(2) BGPAL(512) FRAME(38400) OBJPAL(512) OAM(1024)
//   OBJTILES(16384, from 0x06014000) "@@END\n"
static void uart_send_screenshot() {
  uint16_t dispcnt = REG_DISPCNT;
  const unsigned page = (dispcnt >> 4) & 1;
  uart_write("\n@@SCR1", 7);
  uart_write(&dispcnt, 2);
  uart_write((const void*)0x05000000, 512);
  uart_write((const void*)(0x06000000 + 0xA000 * page), 240 * 160);
  uart_write((const void*)0x05000200, 512);
  uart_write((const void*)0x07000000, 1024);
  uart_write((const void*)0x06014000, 16384);
  uart_write("@@END\n", 6);
}

extern volatile bool uart_xfer_active;     // File transfer running (uart_xfer.c)
static volatile bool uart_xfer_req = false;
void uart_xfer_mode();
void browser_refresh_after_xfer();

static void uart_poll() {
  if (uart_xfer_active)
    return;         // The transfer code owns the UART

  // Bounded, in case the flag never clears (ie. no UART hardware/emulation).
  for (unsigned n = 0; n < 8 && !(REG_SIOCNT & (1 << 5)); n++) {   // Receive FIFO not empty
    uint8_t c = REG_SIODATA8 & 0xFF;
    if (c == 'P')
      uart_send_screenshot();
    else if (c == 'X')
      uart_xfer_req = true;     // Handled by the menu loop (needs FatFs)
    else if (c == '!') {
      // Map the firmware flash back and reboot through the BIOS.
      set_supercard_mode(MAPPED_FIRMWARE, false, false);
      launch_reset(true, false);
    }
    else if (c == '[') {
      uart_in_combo = true;
      uart_combo = 0;
    }
    else if (c == ']') {
      uart_in_combo = false;
      uart_enqueue(uart_combo);
    }
    else if (uart_in_combo)
      uart_combo |= uart_key_for(c);
    else
      uart_enqueue(uart_key_for(c));
  }

  if (uart_state == 1) {          // Pressed: release once read
    if (uart_keys_seen) {
      uart_keys = 0;
      uart_keys_seen = false;
      uart_state = 2;
    }
  }
  else if (uart_state == 2) {     // Released: next key once read
    if (uart_keys_seen)
      uart_state = 0;
  }
  else if (uart_qh != uart_qt) {
    uart_keys = uart_q[uart_qh];
    uart_qh = (uart_qh + 1) % (sizeof(uart_q) / sizeof(uart_q[0]));
    uart_keys_seen = false;
    uart_state = 1;
  }
}
#endif

// Keys held during the last V-blank, and keys pressed since the menu last
// read them (see get_keypress). Sampling here means no press is lost, even
// when the menu takes several frames to do something.
volatile uint16_t keys_held = 0, keys_pressed = 0;

void irq_handler_fn() {
  // Clear all IRQs just in case
  REG_IF = 0xFFFF;
  // Gets called on every V-blank IRQ.
  frame_count++;
  #ifdef ENABLE_UART_LOGGING
    uart_poll();
  #endif

  uint16_t k = REG_KEYINPUT ^ 0x3FF;
  #ifdef ENABLE_UART_LOGGING
    k |= uart_keys;
    uart_keys_seen = true;
  #endif
  keys_pressed |= k & ~keys_held;
  keys_held = k;
}

// Sleeps (CPU halted) until the next V-blank.
static void wait_next_frame() {
  unsigned f = frame_count;
  while (frame_count == f)
    __asm__ volatile ("swi 0x02" ::: "r0", "r1", "r2", "r3", "memory");
}

uint32_t systime() {
  return (frame_count * 50) / 3;
}

static int main_gba() {
  // Setup (if enabled) the SIO in UART mode so we can log stuff.
  #ifdef ENABLE_UART_LOGGING
    REG_RCNT   = 0x0000;
    REG_SIOCNT = 0x0000;
    REG_SIOCNT = 0x3D83;  // UART MODE (115200bps, 8N1, FIFO, send+receive)
  #endif

  // Setup WAITCNT for faster SD-card access.
  REG_WAITCNT = 0x40c0;    // 0x8-0x9: Use 4/2 waitstates (default, slow for SDRAM)
                           // 0xA-0xB: Use 2/1 for fast SD interface access

  // Video is configured in Mode 4 with the logo rendered on the screen.

  // Setup the ROM mapping to allow SD driver. Allow SDRAM usage (as buffer)
  set_supercard_mode(MAPPED_SDRAM, true, true);

  // This hangs on failure since it is fatal.
  init_sdcard_and_mount();

  // Check if we need to save SRAM before doing anything else.
  check_pending_saves();

  // Check if there's a pending SRAM test and perform it.
  int sram_tres = check_peding_sram_test();

  // Load settings files
  load_settings();

  // Load patchdb info.
  set_supercard_mode(MAPPED_SDRAM, true, false);
  memset(&pdbinfo, 0, sizeof(pdbinfo));
  patchmem_dbinfo((uint8_t*)ROM_PATCHDB_U8, &pdbinfo.patch_count, pdbinfo.version, pdbinfo.date, pdbinfo.creator);
  set_supercard_mode(MAPPED_SDRAM, true, true);

  // Configure video mode so we can render the menu.
  setup_video();

  // Setup the IRQ handler, we track V-Blank interrupt (to count frames)
  REG_DISPSTAT |= DISPSTAT_VBLANK_IRQ;
  REG_IRQ_HANDLER_ADDR = (uintptr_t)&gba_irq_handler;
  REG_IF = 0xFFFF;
  REG_IE = 0x0001;
  REG_IME = 1;
  set_irq_enable(true);

  // Initialize menu, start displaying some UI to the user.
  menu_init(sram_tres);

  menu_render(1);
  menu_flip();

  unsigned prev_frame = frame_count;
  bool redraw = true;
  #ifdef ENABLE_UART_LOGGING
    unsigned last_beat = frame_count;
    // Render cost, reported with the heartbeat (timer 2: 64 cycle ticks,
    // 4389 ticks per frame).
    *(volatile uint16_t*)0x0400010A = 0x81;
    unsigned rnd_cnt = 0, rnd_sum = 0, rnd_max = 0;
  #endif
  while (1) {
    #ifdef ENABLE_UART_LOGGING
      if (uart_xfer_req) {
        uart_xfer_req = false;
        uart_xfer_mode();
        browser_refresh_after_xfer();
        redraw = true;
      }
      // Heartbeat, so the serial link can be checked (RX LED blinks every second).
      if (frame_count - last_beat >= 60) {
        last_beat = frame_count;
        WRITE_LOG("alive %u (renders %u, avg %u%% max %u%% of a frame)", frame_count / 60,
                  rnd_cnt, rnd_cnt ? rnd_sum * 100 / 4389 / rnd_cnt : 0, rnd_max * 100 / 4389);
        rnd_cnt = rnd_sum = rnd_max = 0;
      }
    #endif
    uint16_t mkeys = get_keypress();
    if (mkeys) {
      menu_keypress(mkeys);
      redraw = true;
    }
    // Deferred work (ie. loading box art once the cursor rests).
    if (menu_tick())
      redraw = true;

    // Only draw a new frame when something changed or is animating.
    unsigned cframe = frame_count;
    if (redraw || menu_animating()) {
      #ifdef ENABLE_UART_LOGGING
        uint16_t t0 = *(volatile uint16_t*)0x04000108;
      #endif
      if (redraw) {
        menu_invalidate();
        menu_render(cframe - prev_frame);
      } else
        menu_render_idle(cframe - prev_frame);
      #ifdef ENABLE_UART_LOGGING
        unsigned dt = (uint16_t)(*(volatile uint16_t*)0x04000108 - t0);
        rnd_cnt++;
        rnd_sum += dt;
        rnd_max = MAX(rnd_max, dt);
      #endif
      wait_next_frame();    // Flip during V-blank, avoids tearing.
      menu_flip();
      redraw = false;
    } else
      wait_next_frame();
    prev_frame = cframe;
  }

  return 0;
}

static int main_nds() {
  // Video is already ON and inited along with some basic initialization.

  // Mount SD card.
  set_supercard_mode(MAPPED_SDRAM, true, true);
  init_sdcard_and_mount();

  // Check if we need to save SRAM before doing anything else.
  check_pending_saves();

  // Proceed to load BOOT.NDS from disk, if exists
  unsigned errc = load_nds("/BOOT.NDS", (void*)dldi_payload);

  if (errc)
    fatal_init_error("Cannot load BOOT.NDS: %d", errc);

  // Proceed with ARM7 sync and reset to homebrew.
  nds_launch();

  // Should never reach here :D
  while (1);

  return 0;
}

int main() {
  // Detect whether we are running on GBA or NDS.
  isgba = !running_on_nds();
  // Similarly detect if EWRAM seems overclockable (GBA but not micro).
  fastew = test_fast_ewram();

  // Take a look at what flash we have.
  flash_identify(&flashinfo);

  if (isgba)
    return main_gba();
  else
    return main_nds();
}

