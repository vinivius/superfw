#!/usr/bin/env python3
# ipascan.py DISASM: finds call sites that read a low register (r0-r3) right
# after calling a function whose epilogue clobbers it (pop {rN}; bx rN). GCC's
# -fipa-ra assumes such registers preserved across Thumb-1 interworking
# returns (the build uses -fno-ipa-ra for this). DISASM is the output of
#   arm-none-eabi-objdump -d --no-show-raw-insn firmware.ewram.elf
import re, sys
lines = open(sys.argv[1]).read().splitlines()
funcs, cur = {}, None
for l in lines:
    m = re.match(r"^([0-9a-f]+) <([^>]+)>:", l)
    if m:
        cur = m.group(2); funcs[cur] = []; continue
    m = re.match(r"^\s+([0-9a-f]+):\s+(.*)$", l)
    if m and cur:
        funcs[cur].append((int(m.group(1), 16), m.group(2).split(";")[0].split("@")[0].strip()))
clob = {}
for f, ins in funcs.items():
    for i in range(len(ins) - 1):
        m = re.match(r"pop\s+\{(r[0-3])\}", ins[i][1])
        if m and re.match(r"bx\s+" + m.group(1) + r"$", ins[i + 1][1]):
            clob.setdefault(f, set()).add(m.group(1))
bad = []
for f, ins in funcs.items():
    for i, (a, t) in enumerate(ins):
        m = re.match(r"bl\s+[0-9a-f]+ <([^>+]+)>", t)
        if not m or m.group(1) not in clob:
            continue
        for reg in clob[m.group(1)]:
            for a2, t2 in ins[i + 1:i + 12]:
                parts = t2.split(None, 1)
                if len(parts) < 2: break
                mn, args = parts
                regs = re.findall(r"\b(r\d+|ip|lr|sp|pc|fp|sl)\b", args)
                if reg not in regs:
                    if mn.startswith("b") and not mn.startswith("bic"): break
                    continue
                if mn.startswith(("ldm", "stm")):
                    # The base register is read; the list is written (ldm) or read (stm).
                    base = re.match(r"(r\d+|ip|lr|sp|fp|sl)", args).group(1)
                    if base == reg or mn.startswith("stm"):
                        bad.append((f, hex(a), m.group(1), reg, t2))
                    break
                if mn.startswith("pop"):
                    break                       # Loads it: a write
                writes = regs[0] == reg and not mn.startswith(("str", "cmp", "cmn", "tst", "push", "bx", "blx"))
                if not writes or reg in regs[1:]:
                    bad.append((f, hex(a), m.group(1), reg, t2))
                break
print(len(clob), "functions return via pop {r0-r3}; bx;", len(bad), "call sites read the clobbered register")
for b in bad: print(b)
