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
  are within ~1 KiB of the limit: check `stat -c %s superfw.gba` (< 524288)
  and keep debug features small.
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
- `!` also works while a GBA game runs (verified on hardware with Pokemon
  FireRed: back in the menu ~8 s later). It goes through the in-game menu
  IRQ hook (`uart_dbg_poll` in `src/ingame.S`), so it needs the in-game menu
  to be loaded for the game (`igm` in the "Load sizes" log line). Games
  reset or reconfigure the link port (Pokemon games probe for the Wireless
  Adapter at boot), so the hook sets the UART up again whenever it is not in
  UART mode and sends a `~` each time: a burst of `~` after a game starts
  means the hook runs. Link play doesn't work in debug builds.
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
3. `!` (menu or in-game) maps the flash and reboots: that's the flashed
   firmware, not the test build.
4. It adds itself to the Recent list; that is harmless.

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
- Don't rebuild or replace the core while an emulator uses it (SIGBUS).

## Firmware memory budgets

- Flash: 512 KiB for the SD board (`stat -c %s superfw.gba` < 524288); the
  UART build is the tight one. `#pragma GCC optimize("Os")` on cold files
  saved 5 KiB; `COMPRESSION_RATIO=9` saves ~1 KiB more but adds ~45 s per
  build.
- IWRAM: 32 KiB, of which the stack keeps 16 KiB (`ldscripts/gba_ewram.ld`
  asserts "Not enough free IWRAM for stack"). Check the "IWRAM:" line of the
  build. Put big buffers/state in EWRAM with `EWRAM_BSS` (compiler.h): that
  section is NOT zeroed at boot, clear it yourself.
- The in-game menu (`ingamemenu.payload`) shares files with the menu
  (font_render.c, save.c, utf_util.c...). After touching them, compare its
  payload with a build of the previous commit (`git worktree add` + `make
  ingamemenu.payload`) to know whether the in-game menu changed and needs
  testing (open it in the emulator with `keys.sh DIR '<LRs20>'`).

## Known hardware behaviour (Supercard SD)

- SD write CRC status token arrives 2 or 3 clocks after the data at random:
  the start bit is scanned for (`supercard_io.S`, `directsaver.S`).
- SDRAM writes are occasionally dropped (seen at 0x200000, 0x800000,
  0x1000000): ROM loading verifies and rewrites each chunk, then checksums
  the whole ROM (`loader.c`, "Verify ROM loading" setting).
- Fast ROM loading through the 0x0A000000 mirror is unreliable on some
  carts: there is an automatic fallback to slow loading.
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
