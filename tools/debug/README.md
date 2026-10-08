# Debugging tools

Tools to debug SuperFW on real hardware over a serial cable, and in an
emulator. `.claude/skills/superfw-debug/SKILL.md` has the full workflow.

Build the firmware with `make BOARD=sd ENABLE_UART_LOGGING=1`: it logs over
the link port UART (115200 8N1) and accepts commands back (keys, reboot,
screenshots, file transfer).

Cable: a USB to 3.3V UART adapter (ie. CP2102) wired to a link cable plug:
GBA SO (pin 2) to adapter RXD, GBA SI (pin 3) to adapter TXD, and GND (pin
6; cables without that wire can use the battery negative terminal instead).

- `gba-rawlog.py`     Logs the UART (timestamped, reconnects), saves screenshots as PNGs.
- `gba-serial.py`     Sends keys/commands; ls/get/put/rm files on the SD card.
- `gba-shot.sh`       Sends keys, then takes a screenshot and prints its path.
- `gba-keys.sh`       Sends keys, then prints the new log lines.
- `setup-toolchain.sh` Downloads the Arm GNU toolchain.
- `ipascan.py`       Checks a disassembly for the GCC -fipa-ra miscompile the
                      build avoids with -fno-ipa-ra (see the skill).
- `emu/setup.sh`      Builds gpsp with Supercard and UART emulation, plus a
                      headless frontend (`emu/fe.c`); `emu/run.sh` runs it,
                      `emu/keys.sh` injects keys, `emu/shot.sh` saves the
                      exact frame, `emu/montage.py` makes contact sheets.
- `emu/demo-card.py`  Made-up games with original covers for screenshots
                      (README, docs): never show real game names or box art.
