#!/usr/bin/env python3
# Prints the checksum of a bundled file (as checksum_words() in src/asmutil.S
# computes it) as assembler words, for its entry in rom_boot.S: the loader
# checks the unpacked file against it.
import struct, sys

data = open(sys.argv[1], "rb").read()
if len(data) % 4:
    sys.exit(f"{sys.argv[1]}: the size must be a multiple of 4")
a = b = 0
for (w,) in struct.iter_unpack("<I", data):
    a = (a + w) & 0xFFFFFFFF
    b = (b + a) & 0xFFFFFFFF
print(f".word 0x{a:08x}, 0x{b:08x}")
