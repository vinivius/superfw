---
name: superfw-debug
description: Debug SuperFW on a real GBA + Supercard over the link port serial cable (live logs, key injection, menu screenshots, reboot, SD card file transfer, flashing firmware) or in the gpsp emulator. Use when building and testing firmware changes, investigating hardware failures (saves, ROM loading, freezes, slow menu), or operating the GBA remotely.
---

# Debugging SuperFW

Debug builds (`ENABLE_UART_LOGGING=1`) log over the GBA link port UART and
accept commands back, so the GBA can be driven entirely from the PC. The
tools live in `tools/debug/` (see its README for the cable wiring).

**Keep this skill current.** When you find a new quirk, workaround or gotcha,
or improve a tool, update this file (and `tools/debug/`) in the same session
and commit it as its own commit. Push only when the user asks.

## Setup (once)

- Toolchain: `tools/debug/setup-toolchain.sh` installs arm-none-eabi into
  `$SUPERFW_DEV/toolchain` (default `~/Work/superfw-dev`). Then
  `export PATH=$HOME/Work/superfw-dev/toolchain/bin:$PATH`.
- Python: `pyserial`. The serial adapter is `/dev/ttyUSB0` (override with
  `GBA_PORT`); the user needs to be in the `uucp`/`dialout` group.
- Emulator (optional): `tools/debug/emu/setup.sh`.

## Build

    make clean && make BOARD=sd ENABLE_UART_LOGGING=1 -j8     # -> superfw.gba

- Always `make clean` when changing build flags.
- The SD board firmware must fit 512 KiB (enforced at link time). UART builds
  are tight (v0.2: UART ~300 bytes free, release ~2.3KiB), keep debug features small.
  `superfw.gba` is padded to the next 512 byte block (`tools/fw-fixer.py`),
  so `stat` doesn't show the free space;
  measure where the content ends with `tools/debug/flash-free.sh superfw.gba`
  (CI reports it for both builds in the job summary).
- `superfw-next` is protected: changes go through pull requests, built by
  CI (`.github/workflows/superfw-next.yml`). Bump `VERSION_WORD` in any PR
  that changes the release firmware (CI compares release builds of the base
  and the PR made with `VERSION_SLUG_WORD=00000000`); merging publishes the
  release `next-vX.Y` (`tools/ci/publish-release.sh`). Change the release
  scripts together with `tools/ci/test-release.py`, which runs them against
  a fake gh (`python3 tools/ci/test-release.py`, needs jq).

- `ENABLE_DISK_LOGGING=1` writes `/superfwlog.txt` on the SD card instead;
  it is slow and changes SD timing, prefer UART logging.
- The version hash on the Info tab is git HEAD at build time; uncommitted
  changes don't change it. The heartbeat's `[src/main.c:LINE]` also tells
  builds apart.

## Serial link

Start the listener once, in the background (one per port):

    python3 tools/debug/gba-rawlog.py      # log: ~/.cache/superfw-debug/raw.log

It timestamps everything, reconnects if the adapter is unplugged, renders
screenshots to `~/.cache/superfw-debug/shots/*.png`, and pauses while
`gba-serial.py` transfers files (per-port lock in `$XDG_RUNTIME_DIR`). Check
it runs with `pgrep -af gba-rawlog`.

In the menu the firmware logs `[src/main.c:NNN] alive N` every second; N
restarts from 1 after a reboot. Builds from `superfw-next` on add
`(renders R, avg A% max M% of a frame)`: how many frames were drawn in that
second and their CPU cost (idle with nothing animating: 0 renders). Use it
to benchmark menu changes, in the emulator and on hardware. To see what happened after an action, read
the log from its size before the action (`tail -c +OFFSET`, as
`gba-keys.sh` does), not its last lines: older lines are easy to misread as
current.

### Commands (menu only)

| Char | Action |
|---|---|
| `a b u d l r L R s e` | A, B, Up, Down, Left, Right, L, R, Start, Select |
| `[...]` | Combo held together, ie. `[dbs]`, `[LRu]` |
| `!` | Reboot into SuperFW (also from GBA games, see below) |
| `P` | Screenshot (~5 s; lost if sent while booting or flashing: retry). Sent from the V-blank IRQ, so it stalls everything for 5 s: never take one while measuring timings |
| `X` | File transfer mode (used by `gba-serial.py`) |

- The UART receive FIFO is 4 bytes, read once per frame: always send through
  `gba-serial.py send` (35 ms between characters). Bursts lose characters,
  and a lost `]` swallows every following key into a never-ending combo.
- Each injected key stays pressed until the menu reads it, so presses are
  not lost while the menu is busy.
- Be careful with `a` on a ROM: it launches it. In GB/GBC/NES games (run
  through bundled emulators) nothing listens to the UART, so only a power
  cycle by the user gets back to the menu.
- In-game `!` (reboot from a running GBA game) needs a build with
  `UART_INGAME_CONTROL=1`; plain debug builds leave the link port to the
  game, so they are safe to play with. It goes through the in-game menu IRQ
  hook (`uart_dbg_poll` in `src/ingame.S`), so it also needs the in-game
  menu loaded for the game (`igm` in the "Load sizes" log line), and it only
  works in games that don't touch the link port (the hook sets the UART up
  while the port is untouched, and gives it up for good once the game uses
  it). Never make the hook take the port back from a game: a game waiting
  for a transfer polls SIOCNT bit 7, which never clears in UART mode, so it
  freezes at random (Pokemon, Mario Kart at boot: link cable / Wireless
  Adapter probes). The emulator completes transfers at once, so it can't
  show this; a burst of `~` in old logs was this fight.
- After `!`, wait for `Loaded recently played games` and `alive 1` in the
  log (~8 s); a quick look at the last heartbeat can still show the old
  count.

### Tools (`tools/debug/`)

- `gba-shot.sh [KEYS [WAIT]]`: sends keys, waits, takes a screenshot and
  prints the PNG path. Read the PNG to see the screen. Verify every step this
  way; never assume a key arrived.
- `gba-keys.sh KEYS [WAIT]`: sends keys, prints the new log lines.
- `gba-serial.py ls DIR | get REMOTE [LOCAL] | put LOCAL REMOTE | rm REMOTE`:
  SD card access while the menu runs (~11 KB/s on hardware; 4 KiB blocks
  with checksums and acks; `put` writes `NAME.part` then renames). Not
  available while a game runs or during flashing.

## Recipes

### Try a build on hardware without flashing (brick-safe)

A SuperFW image launched from the SD card like a game runs from SDRAM (the
bootloader detects this, `rom_boot.S`), so a broken build can't brick
anything: a power cycle boots the flashed firmware again. Always do this
before flashing a build.

1. `gba-serial.py put superfw.gba /superfw-next.gba` (`.gba`, not `.fw`), and
   read it back with `get` + `cmp`.
2. Browser (or Recent) -> `superfw-next.gba` -> A -> A (no patch prompt for
   SuperFW images). Its heartbeat format tells it apart.
3. `!` (menu, or in-game with `UART_INGAME_CONTROL=1`) maps the flash and
   reboots: that's the flashed firmware, not the test build.
4. It adds itself to the Recent list. When done, `gba-serial.py rm
   /superfw-next.gba` and remove the entry (Recent tab: SELECT, Yes), so the
   card only keeps the real firmware file.
5. Tests read the key register directly and may need a physical press:
   builds from `superfw-next` on also accept START sent over serial to
   abort long operations (memory tests, benchmarks).

### Flash a firmware over serial

1. `gba-serial.py put superfw.gba /superfw-sd-uart.fw` (~50 s). Keep only one
   `.fw` on the card (delete old ones with `rm`), so the right one is picked.
   Read it back with `get` and `cmp` it before flashing (another ~50 s).
   Builds can't be compared with each other: the firmware is compressed and
   embeds the git hash, so a new commit changes almost every byte.
2. Info tab: tabs are Recent (if enabled and not empty), Browser, Settings,
   UI/Language, Tools, Info. Builds from `superfw-next` on wrap around (L on
   Recent goes to Info; older builds stop at the ends), so count presses from
   a known tab and check the screenshot.
3. `gba-shot.sh '[dbs]'`: bottom bar must read "Update flashing is enabled".
4. `gba-shot.sh LLLL 3` (from Info): file browser. Move to the `.fw` with
   `d`/`u` (the header shows position/total) and check that it is
   highlighted. Entries starting with a dot are hidden when "Show hidden
   files" is off, which shifts positions. Builds from `superfw-next` sort
   correctly (GB before GBA, older ones don't), show the path in the header,
   and reopen the browser where the last game was launched from.
5. `gba-shot.sh a 2`: "Firmware update ... Press L+R+Up to flash".
6. `gba-serial.py send '[LRu]'`, then wait ~40 s without sending anything
   (heartbeats pause while flashing). Screenshot: "Flash update complete!".
   If anything else shows, stop and tell the user before rebooting.
7. `gba-serial.py send '!'`, wait ~10 s: the heartbeat restarts at `alive 1`.

Steps can be batched (`gba-shot.sh` prints only the last screenshot), but
look at the unlock and "Firmware update" screenshots before pressing L+R+Up.

### Other

- Card log of disk logging builds: `gba-serial.py get /superfwlog.txt`.
- Bulk changes on the card (thousands of files) are much faster with the
  card in the PC: ask the user to move it.

## Emulator

gpsp with Supercard SD emulation, patched (`emu/gpsp-supercard.patch`) to
expose the UART as a pty and to keep SD image writes.

    truncate -s 256M DIR/sdcard.img && mkfs.vfat -F 32 DIR/sdcard.img
    tools/debug/emu/run.sh DIR superfw.gba [MINUTES] &         # real time
    export GBA_PORT=$(cat DIR/uart.pty) GBA_LOG=DIR/raw.log
    python3 tools/debug/gba-rawlog.py &                         # then the same tools

- `tools/debug/emu/keys.sh DIR KEYS` injects keys straight into the emulator
  (same characters as the UART commands; works with release builds and in
  games). `tools/debug/emu/shot.sh DIR [OUT.png]` saves the exact frame (menu
  or game). `montage.py OUT.png IN.png...` puts many screenshots on one sheet,
  which is cheaper to review than one image at a time.
- `a` on the Recent tab launches the game at once; restart the emulator to
  get back (the menu `!` can't reboot in gpsp).
- For realistic tests, copy a full card image with `cp --reflink=always`
  (instant on btrfs; `/tmp` is a small tmpfs, keep big images in
  `$SUPERFW_DEV`).
- Add files to an image with `udisksctl loop-setup -f DIR/sdcard.img`
  (mounts it); unmount and `udisksctl loop-delete` before running the
  emulator. The emulator writes to the image, so work on a copy.
- Interactive: `cd DIR && retroarch -L $SUPERFW_DEV/gpsp-supercard/gpsp_libretro.so superfw.gba`.
  To show the user a test on screen while you drive it,
  `tools/debug/emu/pad.py STEP...` presses buttons through RetroArch's
  Network RetroPad (needs the retroarch.cfg settings in its header), ie.
  `pad.py down*2 a 1.5 shot:2-open b`. Screenshot after each step that can
  open a prompt: a press meant for a prompt that didn't appear lands on the
  next screen (and may launch a game). RetroArch sometimes segfaults at
  startup (empty log, often right after another instance was killed):
  `pad.py` then stops with "RetroArch is not running"; start it again.
- RetroArch itself may segfault when killed (`timeout`, `pkill`), which
  raises a "Process crashed: retroarch" notification: if
  `coredumpctl info PID` shows frames in `retroarch`, not in
  `gpsp_libretro.so`, it isn't the firmware or the core.
- `GPSP_SIO_TRACE=1` (environment of `run.sh`) prints every SIOCNT/RCNT
  write and every Supercard mode write to stderr: shows what a game does to
  the link port, and whether a reset reached the cartridge mode switch.
- Limitations: interpreter only (the dynarec bypasses the Supercard hooks);
  its built-in BIOS lacks the hard reset (`swi 0x26`), so the menu's `!`
  hangs (the in-game `!` uses a soft reset and works); SD timing is ideal,
  so the hardware quirks below don't reproduce.
- Don't write off an emulator crash or hang as an emulator limitation
  without checking: the in-game `!` crashed gpsp for the same reason it
  froze the GBA.
- Screenshots for the README or docs: use a card made with
  `tools/debug/emu/demo-card.py` (made-up games, original covers), never
  real ROM names or downloaded box art (publishers' copyright). The emulator
  writes to the card image (patch answers, settings): keep a pristine copy
  and restore it before each capture run. Take list shots right after moving
  the cursor, before long names start scrolling.
- SD fault injection (`gba_memory.c`): a file in the emulator folder (the
  core's working directory) makes SD accesses fail from then on.
  `sd-write-fail` holds N: after N more data blocks, every write gets a CRC
  error token and isn't stored (until the emulator exits). `sd-read-fail`
  holds "N K": after N more read commands, the next K never send data (the
  host times out), then reads work again. Use them to test save paths
  (sweep N to hit each step), as done for the in-game save and pending-save
  fixes. An empty `sdram-dump` file makes the core write the cart's SDRAM
  (32 MiB, ROM space order) to `sdram.bin` at the next frame: ie. pad a ROM
  with 0xFF and check its padding after an in-game menu save (the R8-19
  test: SD register writes made with the SDRAM writable change it).
  Rebuild the core with `HAVE_DYNAREC=0` (as setup.sh does).
- Don't rebuild or replace the core while an emulator uses it (SIGBUS).
- Long names: the browser can't open a path over 255 chars ("could not
  load ROM!"). The names made from a ROM name (config, patch file, save,
  savestate) are built by derived_fn(): when they don't fit (the FAT limit
  for config and patch files, MAX_FN_LEN minus the suffixes added later for
  saves and savestates) they are cut short and end in "~" and a hash.
- SD read fault injection: each failing read takes ~1 s to time out, and
  the card keeps failing until K reads failed. A K larger than what the
  test reads leaves the card failing afterwards, which looks like the
  firmware never recovers: use a small K (2 covers a fast and a slow load
  attempt). Keys injected during that timeout are lost: wait ~3 s after the
  action that hits the fault before the next key.
- Counting reads to place a fault: a read command covers at most one FAT
  cluster, and `mkfs.fat` gives a 128 MiB FAT32 image 512-byte clusters
  (an 8 KiB ROM chunk is 16 reads). Format a 4 GiB sparse image with
  `-s 64` (32 KiB clusters) to make a chunk one read, then N ~ the chunk
  number (the fonts start at chunk 1856, 14.5 MiB).
- `run.sh`'s MINUTES stops the emulator: a load that seems stuck at the
  same progress for longer than that is just the last frame. And when
  waiting on the UART log, "ROM load failed" also matches the slow retry's
  "Fast ROM load failed (n)": match "ROM load failed:" for the final one.

## Firmware memory budgets

- Flash: 512 KiB for the SD board (`stat -c %s superfw.gba` < 524288); the
  UART build is the tight one, so it is compressed at upkr level 15 by
  default, the most it does (~140 s per build instead of ~12; level 11 was
  ~110 s and ~120 bytes bigger, level 9 ~65 s and ~320 bigger). Library
  routines are big for one call: strstr() was ~1.5KiB, memchr()/strspn()/
  strpbrk() ~250 bytes; a loop is smaller (see the linker map: build with
  -Wl,-Map and look at the libc/libgcc objects). Compression varies: smaller code can compress worse by ~100
  bytes, so keep a margin. `COMPRESSION_RATIO=4` (~12 s) no longer fits the
  UART build and leaves the release ~60 bytes; use it for quick release-build
  iterations only (the default release level is 9). Cold files use
  `#pragma GCC optimize("Os")` (grep for `optimize *("Os")`, some files
  write it with a space). Check sizes after every change: the UART build had
  dropped to a few dozen bytes free; sha256.c (always) and nanoprintf.c
  (UART builds only, `#ifdef ENABLE_UART_LOGGING`) went -Os to get ~1.1 KiB
  back. In v0.2 the release has ~2.3KiB free (upkr level 9) and the UART
  build ~300 (level 15);
  code compresses poorly (a byte of code costs about a byte of flash).
  nanoprintf.c and utf_util.c are built for size in UART builds only. To
  fit, the UART build also lost the diagnostic that listed where SDRAM
  differs from the file after a failed final ROM verify (git log -S
  log_rom_mismatches brings it back for a hardware investigation). The
  ENABLE_DISK_LOGGING build hasn't fit the SD board since v0.2. Measure the
  overflow with a temporary `MAXFSIZE=600` build instead of guessing.
- Bootloader (`rom_boot.S`, draws the boot screen and unpacks the firmware):
  3 KiB, asserted at link time; check `arm-none-eabi-nm -n firmware.elf |
  grep _end_bootloader` (< 0x08000c00, it was 0x08000bb0). Bigger data it
  needs (the NEXT boot art) goes after `assets_end` and is read from the
  ROM. In gpsp the boot screen stays ~2 s (the interpreter unpacks slowly):
  grab it with `shot.sh` ~0.3 s after starting the emulator.
- Render cost on hardware can be ~15-20% higher than the emulator says
  (gpsp timings are approximate): measure scrolling on the GBA (heartbeat)
  before flashing anything that draws more per frame.
- IWRAM: 32 KiB, of which the stack keeps 16 KiB (`ldscripts/gba_ewram.ld`
  asserts "Not enough free IWRAM for stack"). Check the "IWRAM:" line of the
  build. Put big buffers/state in EWRAM with `EWRAM_BSS` (compiler.h): that
  section is NOT zeroed at boot, clear it yourself.
- The in-game menu (`ingamemenu.payload`) shares files with the menu
  (font_render.c, save.c, utf_util.c...). After touching them, compare its
  payload with a build of the previous commit (`git worktree add` + `make
  ingamemenu.payload`) to know whether the in-game menu changed and needs
  testing (open it in the emulator with `keys.sh DIR '<LRs20>'`).
  Its items: Resume, Reset, Save to SD card, Savestates (4th: `ddd` then
  `a`), RTC clock, Cheats. In Savestates, Left from memory slot 1 goes to
  the persistent (disk) slots 1-5, files `/SAVESTATE/<game>.<n>.state`;
  confirmation popups start on "No" (`l` then `a` for Yes). Pre-create
  state files on the card to test slot handling without saving.
  States keep format 0x10000 (older SuperFW versions read them); the game
  (ROM header code and version) is in the header's unused space after the
  magic "GMID" (offset 16; code at 20, version at 24). Another game's state
  shows "Invalid savestate!", a damaged one "Corrupted savestate!". To test
  compatibility, load a state of this build in a `fork/superfw-next` build.

## Known hardware behaviour (Supercard SD)

- SD write CRC status token arrives 2 or 3 clocks after the data at random:
  the start bit is scanned for (`supercard_io.S`, `directsaver.S`).
- SDRAM writes are occasionally dropped (seen at 0x200000, 0x800000,
  0x1000000): ROM loading verifies and rewrites each chunk, then checksums
  the whole ROM (`loader.c`, "Verify ROM loading" setting). The in-game menu,
  patches, payloads, cheats and NOR flashing's scratch copies are checked
  too (`write16_checked()`, `memcpy32_checked()`, `copy_chunk_verified()`);
  UART builds log each chunk rewrite ("Chunk at 0x... needed N extra
  writes").
- Fast ROM loading through the 0x0A000000 mirror is unreliable on some
  carts: there is an automatic fallback to slow loading.
- The cart's SDRAM loses a write now and then (seen on the user's Supercard
  SD: "Chunk at ... needed 1 extra writes" in the load log): every SDRAM
  write that matters goes through a checked, retrying copy
  (copy_verified(), memcpy32_checked(), write16_checked()); a plain copy
  (the font pack's memmove used to be one) fails loads at random. The
  emulator never drops writes and boots the firmware as if flashed, so
  check load paths on hardware too.
- The cart's registers are in the ROM space: the SD card's at offsets 16 MiB
  (write data), 17 MiB (read data) and 24 MiB (commands), the mode register
  in the last half word (0x09FFFFFE). In the emulator every write made while
  SDRAM is writable reaches SDRAM, these too: SD commands overwrite ROM data
  at 24 MiB, mode changes the last half word (v0.2 failed every ROM over 24
  MiB). SD accesses always run with SDRAM writable (DirectSave uses 0xD7 on
  purpose; read-only SD access is untested on hardware): loads record what
  they write to those two words (`reg_words` in `loader.c`), put it back
  before checking the ROM and after their last SD access. The in-game menu
  and DirectSave access the SD card with the SDRAM read-only (mode 0x3,
  `set_sdcard_mode()`, as libgba's and SCFW's SD drivers do): the game in
  the SDRAM can't be written. UART builds log "SD command word
  kept/overwritten" after a load that wrote data at 24 MiB (a ROM over 24
  MiB): whether the cart's SDRAM gets SD commands (the emulator's does).
- ROMs modified by the old SCFW firmware can be misdetected by the patch
  engine; the patch database handles them.
- Box art lives in `/.superfw/art/XX/<ROM file name>.img`, XX = FNV-1a of the
  name modulo 64 (`docs/boxart-format.md`); FatFs searches folders
  linearly, so one big folder made the menu slow. `migrate_flat_art()` in
  `tools/superfw_romlib.py` converts old cards.
- In-game code (`src/ingame.S`) runs from the cartridge SD-RAM, or from
  EWRAM once the in-game menu is open. Code running from the cartridge must
  not switch the cartridge mapping (`set_cpld_mode`): the next instruction
  is fetched from the newly mapped memory. The resets do the switch from
  IWRAM (`clear_and_reset`).
- GCC (Arm GNU 14.3) with -fipa-ra miscompiled Thumb-1 code: it assumed r0-r3
  survive calls to functions that return with `pop {rN}; bx rN` (interworking),
  so a caller read a struct through r0 = the return address (a patch result,
  and a DLDI header byte stored into firmware code). The build uses
  -fno-ipa-ra (BASEFLAGS); `tools/debug/ipascan.py` checks a disassembly for
  such call sites. Symptoms: results that change when a log line is added.
- The build uses -flto: GCC sees across files, and with strict aliasing it
  dropped a uint16_t store into a local struct that was only read back
  through memcpy32_checked()'s uint32_t pointers (Recent showed full paths:
  fname_offset was never stored; right on the host, which has no LTO). The
  code reads structs and buffers through cast pointers in many places, so
  BASEFLAGS has -fno-strict-aliasing. Symptom: a field right on the host
  test, wrong on the GBA; check the disassembly for the missing store.
- Open: Mario Kart Super Circuit with DirectSave shows a blank screen in game.

## Shell gotchas

- `pkill -f PATTERN` can match the invoking shell itself (exit 144): find
  the PID with an anchored `pgrep -f "^python3 ..."` and kill that.
- `timeout` exits 124, and `grep -c` exits 1 on no match: don't chain them
  with `&&`.
- Foreground `sleep` may be blocked by the harness:
  `python3 -c "import time; time.sleep(N)"`.
- Kill emulator side processes with anchored patterns, ie.
  `pkill -f "^$HOME/Work/superfw-dev/fe "`; a pattern that also appears in
  your own command line kills your shell.
- If `git diff` prints `i/` `w/` prefixes, pass
  `--src-prefix=a/ --dst-prefix=b/` when making patches.
