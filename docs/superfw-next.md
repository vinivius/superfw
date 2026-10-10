# superfw-next

Changes on the `superfw-next` branch, on top of upstream SuperFW (up to
e2614ab) and the `search-boxart` branch (ROM search, box art, SD write and
ROM loading fixes). Everything was tested in the gpsp emulator and on a
Supercard SD with a 2245/1237/653 ROM card.

## Name

SuperFW Next, version 0.2: an unofficial fork, SuperFW is by davidgf. The
boot screen and the About tab show the unchanged SUPERFW logo with "NEXT"
under it (original 80s arcade style lettering, `res/next/make_next.py`,
converted by `res/next/next2c.py`). The firmware header is unchanged, so
SuperFW and SuperFW Next can install each other.

## Responsiveness

- No lost key presses: keys are sampled on every V-blank and each press is
  counted, so presses are never dropped or merged, however busy the menu
  is. Only the D-pad repeats (a held A or B no longer launches a game or
  climbs several folders), and holding it speeds up for long lists.
- The menu only redraws when something changes, and sleeps the CPU
  otherwise. While idle on a long (scrolling) name only that row is
  redrawn: 10% of a frame instead of ~120% on hardware.
- Text rendering is about twice as fast (rows are only measured up to the
  cut, the font code runs from IWRAM).
- Box art loads between frames once the cursor rests, never while
  scrolling, and no longer flashes with the wrong colors. The last 8
  images stay in memory and the ones around the cursor are preloaded
  between key presses (more of them ahead of the cursor), so stepping
  through a list or going back and forth shows them instantly.
- Big folders show a "Loading folder... N" counter while they load.
- Search (START in the browser): the wheel always shows a letter, starting
  at A, and the list always matches what the search bar shows. Up/Down pick
  the letter (L/R jump 5), Right moves on to the next one, Left goes back to
  edit the previous one, A/START close the field and B cancels it. Space is
  a real character (shown as `_` on the wheel). The wheel stays responsive
  in big folders (the list filters once it rests, or right away when the
  wheel closes), and long queries are clipped to the bar.

## Design

- The header names the current tab; in the browser it shows the path and
  the position. The browser's bottom bar shows the START (search) and
  SELECT (file options) buttons.
- The browser reopens where the last game was launched from, and going up
  a folder selects the folder you came from.
- ROM extensions are hidden by default (the icon tells the type), with a
  new "File extensions" option.
- Settings save themselves (when leaving the tab, or shortly after a
  change).
- "No patches found, generate them?" is asked once per ROM.
- Clearer popups: "L/R" page arrows, progress bars with a percentage,
  alerts wrap long messages.
- L/R wrap around the tabs. Dot files (ie. macOS `._` files) count as
  hidden.
- All new strings are translated to the 13 shipped languages.

## Reliability

- Saves are written to a temporary file and checked against the SRAM
  before they replace the current one; a full card or failed rename no
  longer destroys the save (main menu and in-game menu). "Save and quit"
  only quits when the save worked.
- A save that could not be written at boot is retried before the next game
  is launched; if it still fails, the launch stops with an error instead of
  erasing it (the save is kept in SRAM and retried on the next boot).
- GB, GBC and NES games are verified like GBA ROMs: the emulator (bundled
  ones against a checksum made at build time), the ROM header and the ROM
  are read back after being copied to the cart's RAM, rewritten when a write
  was dropped, and checked once more as a whole (and read again from the SD
  card with "Verify ROM loading"), instead of starting a corrupted game.
- A load that fails (ie. SD read errors) shows the error and reloads the
  menu data it overwrote (folder, recent list, box art), instead of going
  on with garbage. A big ROM that also overwrote the fonts reboots the menu
  (like after playing a game) and shows the error after it (unless the
  firmware runs from the SD card: then the reboot goes to the installed
  one). The slow retry of a failed fast load keeps the fonts and cheats
  the first try moved, and installs the in-game menu again.
- Each ROM gets its own config again (an upstream change made them share
  one file). "Remember config" and the "don't ask again" answer to the
  patch prompt say when they could not be saved, instead of "Config saved!".
- ROM names up to the FAT limit (255 characters) work: the save, savestate,
  patch and config names made from them are shortened when they don't fit
  (ending in "~" and a hash of the full name, so they stay unique) instead
  of overflowing their buffers. File manager paths are checked too.
- Cheat files next to the ROM (NAME.cht) are loaded; they were found but
  never loaded. Damaged or oversized cheat files no longer crash the menu
  (a cheat takes up to 30 codes; long titles are cut), Windows line endings
  and tabs work, and a cheat that can't be used (or a title without codes)
  is left out, not the whole file.
  Super codes (5) take their value as halfwords (3 a line) and write all of
  them right; button codes (D) check the keys.
- DirectSave, the in-game menu and its cheats are offered only when they fit
  together after the ROM (or in its free space), DirectSave first, instead
  of the load failing. ROMs just under 32 MiB load without patches too.
- ROMs over 24 MiB load: the SD card's registers are in the ROM space, and
  its commands reached the ROM's data (or the in-game menu's) in the cart's
  RAM while it loaded; the ROM check then failed the load. The data there
  is put back after the load's last SD card access.
- The in-game menu, the patches, the DirectSave payload and the cheats are
  checked after they're written to the cart's RAM, like the ROM; so is
  what a NOR write flashes.
- Built with -fno-ipa-ra: the compiler (GCC 14) otherwise assumed some
  registers survive calls they don't, and miscompiled the DLDI patching of
  NDS homebrew (a header byte was written into the firmware instead).
- Patches are applied correctly when writing a game to NOR flash, where a
  patched function can span two of the parts the image is processed in.
  A NOR write that fails (ie. a read error) waits for the flash erase it
  started, so the NOR game list isn't read as empty (a later write could
  then overwrite the other games). Games over 28 MiB only get the in-game
  menu on NOR when their free space holds its trampoline too.
  Patch database entries and patch files that can't be applied are refused,
  and a game's database patches no longer keep the free space found for the
  game opened before (where the in-game menu would go over game data).
- A card error while checking for a save no longer starts the game with a
  blank save that then replaces it. Hand-edited configs and settings files
  are read whole. A damaged pending save file no longer crashes or hangs
  the boot.
- Patch files the patch engine made from a v1 flash table before the fix
  for those (upstream f170dfb) are made again. The file format stays the
  same, so SuperFW and SuperFW Next keep reading each other's files.
- "Generate patches" placed the patches of ROMs over 8 MiB 8, 16 or 24 MiB
  too low (the game then broke where they went); patches cached for such
  ROMs before the fix are made again (generation is offered). A patch set
  that doesn't fit (128 patches) fails generation instead of overwriting
  memory, the save functions of another save type (ie. in compilations)
  no longer count, and a constant's farthest ARM load is found.
- Savestates and generated patches say which game they are for (the ROM
  header's code and version): another game's (a ROM of the same name in
  another folder, or another version of it) isn't used, the state shows
  "Invalid savestate!". The file formats stay the same: older SuperFW
  (Next) versions read them; their states load in any game, and their
  patches in any ROM of that name up to 8 MiB.
- The NOR game table is a log that keeps working after a write cut short
  (it falls back to the newest whole table), never writes past its area and
  refuses NOR writes and deletes while it can't be read, instead of taking
  it as empty (a write could then overwrite the other games).
- The in-game menu and DirectSave use the SD card with the cart's RAM
  read-only (as libgba's SD driver does): their SD card commands and data
  could reach a running game over 16 MiB (or the in-game menu's own data
  there) and change it.
- The in-game menu's font pack is moved checked (in chunks, each written
  again if it doesn't read back): the cart's RAM loses a write now and then,
  and a lost one failed the load ("ROM verification failed").
- "Reset without saving" in the in-game menu stays in the menu and says so
  when it can't cancel the pending save (it would be written at boot).
- A damaged or hand-edited recent.txt (no final newline, overlong lines,
  Windows line endings) no longer crashes or hangs the boot.
- Firmware updates retry (up to 3 times) when erasing, writing or
  verifying fails, and the screen says not to turn the console off.
- Fixed crashes: more than 200 recent games, malformed recent.txt and
  settings files, long ROM names, oversized patch databases.
- Fixed sorting (names that are a prefix of another one, ie. GB vs GBA),
  folders that can't be opened now show an error.
- In-game `!` reset over serial works from games that use the link port
  (UART debug builds), and the in-game resets switch the cartridge mapping
  from IWRAM.

## Debugging

`tools/debug/` and `.claude/skills/superfw-debug/SKILL.md`: serial logging,
key injection, screenshots and file transfer, test-booting builds from the
SD card without flashing, and a gpsp setup (Supercard SD, UART, DirectSave)
with key injection and frame capture.

## Builds and releases

`superfw-next` only accepts pull requests. GitHub Actions
(`.github/workflows/superfw-next.yml`) builds every pull request into it:
the release and the UART debug firmware, with the same Arm toolchain as
`tools/debug/setup-toolchain.sh` (the output is byte-identical to a local
build), plus the unit tests.

A pull request that changes the release firmware must bump `VERSION_WORD`
in the Makefile (`0x00000002` is version 0.2), or the check fails. CI builds
the release for the base and for the pull request with the same commit hash
and compares them, so docs, tools or debug-only changes don't need a bump.
A pull request must be up to date with `superfw-next` to merge, so the
check always runs against the latest version.

Every merge publishes the release `next-vX.Y` for its version, once, in
merge order: `superfw-next-vX.Y-sd.fw` to flash, the same image as `.gba`
to try it from the SD card first, the debug build and `SHA256SUMS`. The
release scripts live in `tools/ci/`; `tools/ci/test-release.py` runs them
against a fake `gh` in every pull request.
