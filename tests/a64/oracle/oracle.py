#!/usr/bin/env python3
"""Unicorn oracle for the lifter tests (run with the repository's .venv).

  oracle.py run --elf fixture.elf [--image module] --lifted [emulator...] program
      Runs the fixture under Unicorn and the lifted program on `module`
      (the ELF unless given: a platform runs the same fixture in its own
      format) through the cross-compiling emulator, if any; their output
      (the fixture's writes) must be identical.
  oracle.py forms --elf fixture.elf --forms forms.txt --out vectors.bin
      Runs every instruction form recomp_lift_a64 found (forms.txt) under Unicorn
      from random and special states and writes the input and output states
      for lift_forms_test.

Unicorn is a test oracle only: it never reaches a product binary.
"""

import argparse
import random
import struct
import subprocess
import sys
import zlib

from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
from unicorn import arm64_const as A

# a64/image.h's kElfBase; fixture_main.cpp
MAIN_BASE = 0x7100000000
STACK_BASE = 0x7000000000
STACK_SIZE = 0x100000
# lift_forms_test.cpp
BUF = 0x7080000000
BUF_SIZE = 0x10000
WINDOW = BUF + BUF_SIZE // 2 - 256
WINDOW_SIZE = 512
PATTERN_SEED = 0x5EED

R_AARCH64_RELATIVE = 1027
XREGS = [getattr(A, f"UC_ARM64_REG_X{i}") for i in range(31)]
MASK64 = (1 << 64) - 1


def page_up(v):
    return (v + 0xFFF) & ~0xFFF


def load_elf(path):
    """The static-PIE ELF's memory image (a64/image.h's LoadElf), relocated
    for MAIN_BASE."""
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or struct.unpack_from("<H", data, 18)[0] != 183:
        sys.exit(f"{path}: not an AArch64 ELF")
    phoff, = struct.unpack_from("<Q", data, 32)
    phentsize, phnum = struct.unpack_from("<HH", data, 54)
    loads, dyn = [], None
    for i in range(phnum):
        ptype, _, off, va, _, filesz, memsz = struct.unpack_from(
            "<IIQQQQQ", data, phoff + i * phentsize)
        if ptype == 1:
            loads.append((off, va, filesz, memsz))
        elif ptype == 2:
            dyn = va
    image = bytearray(page_up(max(va + memsz for _, va, _, memsz in loads)))
    for off, va, filesz, _ in loads:
        image[va:va + filesz] = data[off:off + filesz]
    tags = {}
    while True:
        tag, val = struct.unpack_from("<qQ", image, dyn)
        if tag == 0:
            break
        tags[tag] = val
        dyn += 16
    for at, size in ((tags.get(7, 0), tags.get(8, 0)), (tags.get(23, 0), tags.get(2, 0))):
        for p in range(at, at + size, 24):
            off, info, addend = struct.unpack_from("<QQq", image, p)
            if info & 0xFFFFFFFF != R_AARCH64_RELATIVE:
                sys.exit(f"{path}: relocation type {info & 0xFFFFFFFF} at {off:#x}")
            struct.pack_into("<Q", image, off, (MAIN_BASE + addend) & MASK64)
    return bytes(image)


def new_cpu(image):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    if hasattr(A, "UC_CPU_ARM64_A57"):  # the fixture is built for it
        uc.ctl_set_cpu_model(A.UC_CPU_ARM64_A57)
    uc.mem_map(MAIN_BASE, len(image))
    uc.mem_write(MAIN_BASE, image)
    return uc


def run(args):
    image = load_elf(args.elf)
    uc = new_cpu(image)
    uc.mem_map(STACK_BASE, STACK_SIZE)
    uc.reg_write(A.UC_ARM64_REG_SP, STACK_BASE + STACK_SIZE)
    out = bytearray()
    state = {"exited": False, "error": None}

    def on_intr(uc, intno, _):
        pc = uc.reg_read(A.UC_ARM64_REG_PC)
        insn = struct.unpack("<I", uc.mem_read(pc - 4, 4))[0]
        if insn & 0xFFE0001F != 0xD4000001:
            state["error"] = f"exception {intno} at {pc:#x}"
            uc.emu_stop()
            return
        imm = (insn >> 5) & 0xFFFF
        if imm == 1:  # fixture_write (fixture_platform.cpp)
            x0, x1 = uc.reg_read(A.UC_ARM64_REG_X0), uc.reg_read(A.UC_ARM64_REG_X1)
            out.extend(uc.mem_read(x0, x1))
            uc.reg_write(A.UC_ARM64_REG_X0, 0)
        elif imm == 0:  # exit
            state["exited"] = True
            uc.emu_stop()
        else:
            state["error"] = f"svc {imm:#x} at {pc - 4:#x}"
            uc.emu_stop()

    uc.hook_add(UC_HOOK_INTR, on_intr)
    try:
        uc.emu_start(MAIN_BASE, 0, timeout=300 * 1000 * 1000)
    except UcError as e:
        pc = uc.reg_read(A.UC_ARM64_REG_PC)
        state["error"] = f"{e} at {pc:#x}"
    if state["error"] or not state["exited"]:
        sys.exit(f"oracle: {state['error'] or 'did not exit'}")

    lifted = subprocess.run(args.lifted + [args.image or args.elf], stdout=subprocess.PIPE)
    expect, got = bytes(out), lifted.stdout
    sys.stdout.write("oracle:\n" + expect.decode(errors="replace"))
    if lifted.returncode != 0:
        sys.exit(f"lifted program exited with status {lifted.returncode}")
    if got != expect:
        sys.stdout.write("lifted:\n" + got.decode(errors="replace"))
        sys.exit("output differs")
    print("lifted output matches")


SPECIAL = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000,
           0xFFFFFFFF, 0x100000000, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, MASK64,
           0xFFFFFFFF00000000, 31, 32, 63, 64]


def value(rng):
    k = rng.randrange(10)
    if k < 3:
        return rng.choice(SPECIAL)
    if k == 3:
        return rng.randrange(64)
    if k == 4:
        return (-rng.randrange(1, 300)) & MASK64
    if k == 5:
        return rng.getrandbits(32)
    return rng.getrandbits(64)


def index_value(rng):
    """A register offset that keeps the access inside the window (most of
    the time) under any of the extends."""
    k = rng.randrange(4)
    if k == 0:
        return (-rng.randrange(1, 32)) & 0xFFFFFFFF  # negative under SXTW
    if k == 1:
        return (-rng.randrange(1, 32)) & MASK64
    return rng.randrange(32)


def pattern():
    s, out = PATTERN_SEED, bytearray()
    for _ in range(BUF_SIZE // 8):
        s = (s + 0x9E3779B97F4A7C15) & MASK64
        z = s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
        out += struct.pack("<Q", z ^ (z >> 31))
    return bytes(out)


def forms(args):
    image = load_elf(args.elf)
    uc = new_cpu(image)
    uc.mem_map(BUF, BUF_SIZE)
    base_pattern = pattern()
    clean = uc.context_save()  # no exclusive monitor, as each lifted run starts
    rng = random.Random(args.seed)
    count = skipped = 0
    with open(args.out, "wb") as out:
        out.write(b"A64V0001")
        for line in open(args.forms):
            idx, va, raw, kind, rn, rm, _ = line.split()
            idx, va, raw, rn, rm = int(idx), int(va, 16), int(raw, 16), int(rn), int(rm)
            for _ in range(args.trials):
                regs = [value(rng) for _ in range(31)]
                sp = value(rng)
                if kind == "mem":
                    if rm >= 0 and rm != 31:
                        regs[rm] = index_value(rng)
                    mid = BUF + BUF_SIZE // 2
                    if rn == 31:
                        sp = mid
                    else:
                        regs[rn] = mid
                nzcv = rng.randrange(16) << 28
                tpidr = value(rng)
                window = rng.randbytes(WINDOW_SIZE)
                uc.context_restore(clean)
                uc.mem_write(BUF, base_pattern)
                uc.mem_write(WINDOW, window)
                for r, v in zip(XREGS, regs):
                    uc.reg_write(r, v)
                uc.reg_write(A.UC_ARM64_REG_SP, sp)
                uc.reg_write(A.UC_ARM64_REG_NZCV, nzcv)
                uc.reg_write(A.UC_ARM64_REG_TPIDR_EL0, tpidr)
                try:
                    uc.emu_start(va, va + 4, count=1)
                except UcError:
                    skipped += 1  # faults natively (an address off the buffer)
                    continue
                if uc.reg_read(A.UC_ARM64_REG_PC) != va + 4:
                    sys.exit(f"form {idx} at {va:#x} did not complete")
                after = [uc.reg_read(r) for r in XREGS]
                state_in = struct.pack("<34Q", *regs, sp, nzcv, tpidr)
                state_out = struct.pack(
                    "<34Q", *after, uc.reg_read(A.UC_ARM64_REG_SP),
                    uc.reg_read(A.UC_ARM64_REG_NZCV) & 0xF0000000,
                    uc.reg_read(A.UC_ARM64_REG_TPIDR_EL0))
                crc = zlib.crc32(uc.mem_read(BUF, BUF_SIZE))
                out.write(struct.pack("<II", idx, raw) + state_in + window + state_out +
                          struct.pack("<II", crc, 0))
                count += 1
    print(f"oracle: {count} vectors, {skipped} trials faulted and were skipped")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--elf", required=True)
    r.add_argument("--image")
    r.add_argument("--lifted", required=True, nargs=argparse.REMAINDER,
                   help="the lifted program (and its emulator); must come last")
    f = sub.add_parser("forms")
    f.add_argument("--elf", required=True)
    f.add_argument("--forms", required=True)
    f.add_argument("--out", required=True)
    f.add_argument("--trials", type=int, default=100)
    f.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    {"run": run, "forms": forms}[args.cmd](args)


if __name__ == "__main__":
    main()
