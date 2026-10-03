# SuperFW box-art file format (.img) — shared spec

Location on SD card: /.superfw/art/<full ROM filename>.img
  e.g. ROM "/GBA/Golden Sun (USA).gba" -> "/.superfw/art/Golden Sun (USA).gba.img"
  (lookup is by ROM filename only, directory is ignored)

Layout (all integers little-endian):
  offset 0   char[4]  magic "SFWA"
  offset 4   u16      width   (even, 2..80)
  offset 6   u16      height  (1..80)
  offset 8   u16      ncolors (1..128)
  offset 10  u16      reserved, 0
  offset 12  u16[ncolors]  palette, GBA BGR555: r5 | g5<<5 | b5<<10, bit15 = 0
  then       u8[width*height] pixel indices 0..ncolors-1, row-major, top row first

Notes:
- Image is scaled to fit in 80x80 keeping aspect ratio (no padding/canvas), width rounded to even.
- Firmware loads palette into BG palette entries 96..223 and adds 96 to each pixel index.
- Max file size: 12 + 256 + 6400 = 6668 bytes.
