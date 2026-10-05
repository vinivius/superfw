# superfw-next

Changes on the `superfw-next` branch, on top of upstream SuperFW (up to
bb97fe5) and the `search-boxart` branch (ROM search, box art, SD write and
ROM loading fixes). Everything was tested in the gpsp emulator and on a
Supercard SD with a 2245/1237/653 ROM card.

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
  scrolling, and no longer flashes with the wrong colors.
- Big folders show a "Loading folder... N" counter while they load.
- The search wheel stays responsive in big folders (the list filters once
  the wheel rests), L/R jump 5 letters.

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
