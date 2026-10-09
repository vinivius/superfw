#!/usr/bin/env python3
"""GBA serial link helper (SuperFW UART debug builds).

  gba-serial.py send KEYS            Send key presses: a b u d l r L R s e, [..] combos,
                                     ! reboot, P screenshot.
  gba-serial.py ls [DIR]             List a directory on the SD card.
  gba-serial.py get REMOTE [LOCAL]   Copy a file from the SD card.
  gba-serial.py put LOCAL REMOTE     Copy a file to the SD card.
  gba-serial.py rm REMOTE            Delete a file on the SD card.

The port is /dev/ttyUSB0, or $GBA_PORT (ie. the emulator's pty). While a
transfer runs, gba-rawlog.py is paused through a lock file so it does not
steal bytes.
"""
import os, sys, time, struct, serial

PORT = os.environ.get("GBA_PORT", "/dev/ttyUSB0")
BAUD = 115200
BLK = 4096
LOCK = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"),
                    "superfw-xfer-%s.lock" % os.path.basename(PORT))


def checksum(data):
    """Same as the firmware's checksum_words(), over zero padded LE words."""
    data = data + b"\0" * (-len(data) % 4)
    a = b = 0
    for (w,) in struct.iter_unpack("<I", data):
        a = (a + w) & 0xFFFFFFFF
        b = (b + a) & 0xFFFFFFFF
    return struct.pack("<II", a, b)


class Link:
    """Opens the port (pausing the logger) and enters file transfer mode."""

    def __enter__(self):
        with open(LOCK, "w") as f:
            f.write(str(os.getpid()))      # gba-rawlog.py ignores locks of dead processes
        try:
            time.sleep(0.4)                # Let gba-rawlog.py release the port
            self.s = serial.Serial(PORT, BAUD, timeout=0.2)
            # A session may still be open (ie. an interrupted run): probe it
            # with "?", which gets an "ERR" in transfer mode and is not a key
            # command in the menu. Otherwise start a session with 'X'.
            self.s.reset_input_buffer()
            self.s.write(b"\n?\n")
            if not self.answered(b"ERR", 0.8):
                self.s.write(b"X")
                self.expect_line(b"XFER READY", 10)
            self.s.reset_input_buffer()
        except BaseException:
            if getattr(self, "s", None):
                self.s.close()
            os.unlink(LOCK)
            raise
        return self

    def __exit__(self, *exc):
        try:
            self.cmd("QUIT")
        finally:
            self.s.close()
            os.unlink(LOCK)

    def answered(self, what, timeout):
        end, data = time.time() + timeout, b""
        while time.time() < end:
            data += self.s.read(256)
            if what in data:
                time.sleep(0.2)            # Let any second reply arrive too
                self.s.read(4096)
                return True
        return False

    def readline(self, timeout=10):
        end, line = time.time() + timeout, b""
        while time.time() < end:
            c = self.s.read(1)
            if c == b"\n":
                return line.rstrip(b"\r")
            line += c
        raise TimeoutError("no answer from the GBA (got %r)" % line)

    def expect_line(self, what, timeout):
        end = time.time() + timeout
        while time.time() < end:
            try:
                if self.readline(end - time.time()) == what:
                    return
            except TimeoutError:
                break
        raise TimeoutError("GBA did not enter transfer mode (is it in the SuperFW menu?)")

    def read_exact(self, n, timeout=10):
        end, data = time.time() + timeout, b""
        while len(data) < n and time.time() < end:
            data += self.s.read(n - len(data))
        if len(data) < n:
            raise TimeoutError("transfer stalled")
        return data

    def cmd(self, line):
        self.s.write(line.encode("utf-8") + b"\n")
        return self.readline().decode("utf-8", "replace")

    def ls(self, path):
        r = self.cmd("LS " + path)
        if r != "OK":
            raise RuntimeError(r)
        out = []
        while True:
            l = self.readline().decode("utf-8", "replace")
            if l == "END":
                return out
            if l.startswith("ERR"):
                raise RuntimeError(l)
            out.append(l)

    def get(self, remote, local):
        r = self.cmd("GET " + remote)
        if not r.startswith("OK "):
            raise RuntimeError(r)
        size, done, t0 = int(r[3:]), 0, time.time()
        with open(local, "wb") as f:
            while done < size:
                n = min(BLK, size - done)
                start = self.read_exact(1)
                if start != b"B":
                    raise RuntimeError("bad block start %r" % start)
                data, ck = self.read_exact(n), self.read_exact(8)
                if checksum(data) != ck:
                    self.s.reset_input_buffer()
                    self.s.write(b"N")
                    continue
                f.write(data)
                done += n
                self.s.write(b"A")
                progress(done, size, t0)
        print()

    def put(self, local, remote):
        data = open(local, "rb").read()
        r = self.cmd("PUT %d %s" % (len(data), remote))
        if r != "OK":
            raise RuntimeError(r)
        done, t0 = 0, time.time()
        while done < len(data):
            blk = data[done:done + BLK]
            for _ in range(8):
                self.s.write(b"B" + blk + checksum(blk))
                ack = self.read_exact(1, 30)
                if ack != b"N":
                    break
            if ack != b"A":
                raise RuntimeError("block at %d rejected (%r)" % (done, ack))
            done += len(blk)
            progress(done, len(data), t0)
        print()
        r = self.readline(30).decode()
        if r != "DONE":
            raise RuntimeError(r)


def progress(done, total, t0):
    rate = done / max(time.time() - t0, 0.001) / 1024
    sys.stdout.write("\r  %d / %d bytes (%.1f KiB/s)" % (done, total, rate))
    sys.stdout.flush()


def send(keys):
    # The GBA UART only has a 4 byte receive FIFO, emptied once per frame
    # outside of transfer mode: pace the characters (~2 frames apart).
    with serial.Serial(PORT, BAUD, timeout=0.2) as s:
        for c in keys.encode():
            s.write(bytes([c]))
            s.flush()
            time.sleep(0.035)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd, args = argv[1], argv[2:]
    if cmd == "send" and args:
        send(args[0])
    elif cmd == "ls":
        with Link() as l:
            for e in l.ls(args[0] if args else "/"):
                print(e)
    elif cmd == "get" and args:
        with Link() as l:
            l.get(args[0], args[1] if len(args) > 1 else os.path.basename(args[0]))
    elif cmd == "put" and len(args) == 2:
        with Link() as l:
            l.put(args[0], args[1])
    elif cmd == "rm" and args:
        with Link() as l:
            print(l.cmd("RM " + args[0]))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
