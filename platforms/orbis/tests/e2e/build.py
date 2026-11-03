#!/usr/bin/env python3
"""Builds orbis_process_test's eboot.elf and prx.elf into OUTDIR.

usage: build.py OUTDIR CLANG LD_LLD
"""
import os
import struct
import subprocess
import sys

out, clang, lld = sys.argv[1:4]
here = os.path.dirname(os.path.abspath(__file__))
sce = os.path.join(here, "..", "..", "..", "..", "tools", "orbis_sce_elf.py")
os.makedirs(out, exist_ok=True)
cflags = ["-target", "x86_64-pc-freebsd12", "-fPIC", "-O2", "-ffreestanding", "-funwind-tables",
          "-fno-stack-protector", "-fno-builtin", "-c"]


def run(*cmd):
    subprocess.run(cmd, check=True)


def o(name):
    return os.path.join(out, name)


run(clang, *cflags, "-DLIB_TEST", os.path.join(here, "stubs.c"), "-o", o("test_stub.o"))
run(clang, *cflags, "-DLIB_KERNEL", os.path.join(here, "stubs.c"), "-o", o("kernel_stub.o"))
run(lld, "-shared", "-soname", "libTest", o("test_stub.o"), "-o", o("libTest.so"))
run(lld, "-shared", "-soname", "libkernel", o("kernel_stub.o"), "-o", o("libkernel.so"))
LIBS = (("libSceFiber", "LIB_FIBER"), ("libSceGnmDriver", "LIB_GNM"),
        ("libSceSaveData", "LIB_SAVEDATA"), ("libSceAudioOut", "LIB_AUDIOOUT"),
        ("libScePad", "LIB_PAD"), ("libSceNpTrophy", "LIB_TROPHY"), ("libSceNgs2", "LIB_NGS2"),
        ("libSceCommonDialog", "LIB_COMMONDIALOG"), ("libSceSaveDataDialog", "LIB_SAVEDATADIALOG"))
for lib, define in LIBS:
    run(clang, *cflags, "-D" + define, os.path.join(here, "stubs.c"), "-o", o(lib + "_stub.o"))
    run(lld, "-shared", "-soname", lib, o(lib + "_stub.o"), "-o", o(lib + ".so"))

run(clang, *cflags, os.path.join(here, "prx.c"), "-o", o("prx.o"))
run(lld, "-shared", "-soname", "libTestPrx", "--eh-frame-hdr", o("prx.o"), o("libkernel.so"),
    "-o", o("prx.so"))
run(sys.executable, sce, o("prx.so"), o("prx.elf"), "--prx", "--name", "libTestPrx",
    "--lib", "libkernel=" + o("libkernel.so"))

run(clang, *cflags, os.path.join(here, "eboot.c"), "-o", o("eboot.o"))
run(clang, *cflags, os.path.join(here, "kernel.c"), "-o", o("kernel.o"))
run(clang, *cflags, os.path.join(here, "system.c"), "-o", o("system.o"))
run(lld, "-pie", "--no-dynamic-linker", "--eh-frame-hdr", "-e", "_start", o("eboot.o"),
    o("kernel.o"), o("system.o"), o("libTest.so"), o("prx.so"), o("libkernel.so"),
    *[o(lib + ".so") for lib, _ in LIBS], "-o", o("eboot.so"))
run(sys.executable, sce, o("eboot.so"), o("eboot.elf"), "--name", "eboot",
    "--lib", "libTest=" + o("libTest.so"), "--lib", "libTestPrx=" + o("prx.so"),
    "--lib", "libkernel=" + o("libkernel.so"),
    *[a for lib, _ in LIBS for a in ("--lib", lib + "=" + o(lib + ".so"))])


def symbols(path):
    """The ELF's .symtab: name -> value."""
    b = open(path, "rb").read()
    shoff, = struct.unpack_from("<Q", b, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", b, 0x3A)
    sh = [struct.unpack_from("<IIQQQQIIQQ", b, shoff + i * shentsize) for i in range(shnum)]
    out = {}
    for _, typ, _, _, off, size, link, _, _, entsize in sh:
        if typ != 2:  # SHT_SYMTAB
            continue
        stroff = sh[link][4]
        for k in range(off, off + size, entsize):
            name, _, _, _, value, _ = struct.unpack_from("<IBBHQQ", b, k)
            out[b[stroff + name:b.index(b"\0", stroff + name)].decode()] = value
    return out


# The eboot's manifest: its setjmp and longjmp's ret (eboot.c).
sym = symbols(o("eboot.so"))
with open(o("eboot.manifest"), "w") as f:
    f.write("setjmp 0x%x\nresume 0x%x\n" % (sym["test_setjmp"], sym["test_longjmp_ret"]))
