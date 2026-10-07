# superfw-next

Changes on the `superfw-next` branch, on top of upstream SuperFW (up to
5ecb841) and the `search-boxart` branch (ROM search, box art, SD write and
ROM loading fixes). Everything was tested in the gpsp emulator and on a
Supercard SD with a 2245/1237/653 ROM card.

## Name

SuperFW Next, version 0.1: an unofficial fork, SuperFW is by davidgf. The
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
  images stay in memory and the ones around the cursor are preloaded, so
  going back and forth through a list shows them instantly.
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
build), plus the unit tests. A pull request that changes the firmware must
bump `VERSION_WORD` in the Makefile (`0x00000002` is version 0.2), or the
check fails. Every merge publishes the release `next-vX.Y` for that version
(once): `superfw-next-vX.Y-sd.fw` to flash, the same image as `.gba` to try
it from the SD card first, the debug build and `SHA256SUMS`.
