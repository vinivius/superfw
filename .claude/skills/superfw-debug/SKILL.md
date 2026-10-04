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
restarts from 1 after a reboot. To see what happened after an action, read
the log from its size before the action (`tail -c +OFFSET`, as
`gba-keys.sh` does), not its last lines: older lines are easy to misread as
current.

### Commands (menu only)

| Char | Action |
|---|---|
| `a b u d l r L R s e` | A, B, Up, Down, Left, Right, L, R, Start, Select |
| `[...]` | Combo held together, ie. `[dbs]`, `[LRu]` |
| `!` | Reboot into SuperFW (also from GBA games, see below) |
| `P` | Screenshot (~5 s; lost if sent while booting or flashing: retry) |
| `X` | File transfer mode (used by `gba-serial.py`) |

- The UART receive FIFO is 4 bytes, read once per frame: always send through
  `gba-serial.py send` (35 ms between characters). Bursts lose characters,
  and a lost `]` swallows every following key into a never-ending combo.
- Each injected key stays pressed until the menu reads it, so presses are
  not lost while the menu is busy.
- Be careful with `a` on a ROM: it launches it. In GB/GBC/NES games (run
  through bundled emulators) nothing listens to the UART, so only a power
  cycle by the user gets back to the menu.
- `!` while a GBA game runs works through the in-game menu IRQ hook
  (`uart_dbg_poll` in `src/ingame.S`), so only when the in-game menu is
  enabled and the game doesn't use the link port. Not yet confirmed on
  hardware; update this line once it is.

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

### Flash a firmware over serial

1. `gba-serial.py put superfw.gba /superfw-sd-uart.fw` (~50 s). Keep only one
   `.fw` on the card (delete old ones with `rm`), so the right one is picked.
2. `gba-shot.sh RRRRRR 3`: Info tab. Tabs: Recent (if enabled and not
   empty), Browser, Settings, UI/Language, Tools, Info; L/R stop at the ends
   (no wrap), so extra presses are harmless.
3. `gba-shot.sh '[dbs]'`: bottom bar must read "Update flashing is enabled".
4. `gba-shot.sh LLLL 3`: file browser. Move to the `.fw` with `d`/`u` (the
   header shows position/total) and check that it is highlighted.
5. `gba-shot.sh a 2`: "Firmware update ... Press L+R+Up to flash".
6. `gba-serial.py send '[LRu]'`, then wait ~40 s without sending anything
   (heartbeats pause while flashing). Screenshot: "Flash update complete!".
   If anything else shows, stop and tell the user before rebooting.
7. `gba-serial.py send '!'`, wait ~10 s: the heartbeat restarts at `alive 1`.

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

- Add files to an image with `udisksctl loop-setup -f DIR/sdcard.img`
  (mounts it); unmount and `udisksctl loop-delete` before running the
  emulator. The emulator writes to the image, so work on a copy.
- Interactive: `cd DIR && retroarch -L $SUPERFW_DEV/gpsp-supercard/gpsp_libretro.so superfw.gba`.
- Limitations: interpreter only (the dynarec bypasses the Supercard hooks);
  its built-in BIOS can't do a hard reset, so `!` hangs; SD timing is ideal,
  so the hardware quirks below don't reproduce.
- Don't rebuild or replace the core while an emulator uses it (SIGBUS).

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
- Open: Mario Kart Super Circuit with DirectSave shows a blank screen in game.

## Shell gotchas

- `pkill -f PATTERN` can match the invoking shell itself (exit 144): find
  the PID with an anchored `pgrep -f "^python3 ..."` and kill that.
- `timeout` exits 124, and `grep -c` exits 1 on no match: don't chain them
  with `&&`.
- Foreground `sleep` may be blocked by the harness:
  `python3 -c "import time; time.sleep(N)"`.
- If `git diff` prints `i/` `w/` prefixes, pass
  `--src-prefix=a/ --dst-prefix=b/` when making patches.
