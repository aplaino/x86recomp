#!/usr/bin/env python3
"""Builds orbis_process_test's eboot.elf, prx.elf and load.elf into OUTDIR,
and the same laid out as a game's app0 (OUTDIR/app0: eboot.bin,
sce_module/).

usage: build.py OUTDIR CLANG LD_LLD
"""
import os
import shutil
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
        ("libSceCommonDialog", "LIB_COMMONDIALOG"), ("libSceSaveDataDialog", "LIB_SAVEDATADIALOG"),
        ("libSceLibcInternal", "LIB_LIBC"), ("libSceRtc", "LIB_RTC"),
        ("libSceMsgDialog", "LIB_MSGDIALOG"), ("libSceAppContent", "LIB_APPCONTENT"),
        ("libSceAjm", "LIB_AJM"), ("libSceVideodec2", "LIB_VIDEODEC2"),
        ("libSceNpTus", "LIB_NPTUS"))
for lib, define in LIBS:
    run(clang, *cflags, "-D" + define, os.path.join(here, "stubs.c"), "-o", o(lib + "_stub.o"))
    run(lld, "-shared", "-soname", lib, o(lib + "_stub.o"), "-o", o(lib + ".so"))

run(clang, *cflags, os.path.join(here, "prx.c"), "-o", o("prx.o"))
run(lld, "-shared", "-soname", "libTestPrx", "--eh-frame-hdr", o("prx.o"), o("libkernel.so"),
    "-o", o("prx.so"))
run(sys.executable, sce, o("prx.so"), o("prx.elf"), "--prx", "--name", "libTestPrx",
    "--lib", "libkernel=" + o("libkernel.so"))

run(clang, *cflags, os.path.join(here, "load.c"), "-o", o("load.o"))
run(lld, "-shared", "-soname", "libTestLoad", "--eh-frame-hdr", o("load.o"), "-o", o("load.so"))
run(sys.executable, sce, o("load.so"), o("load.elf"), "--prx", "--name", "libTestLoad")

run(clang, *cflags, os.path.join(here, "eboot.c"), "-o", o("eboot.o"))
run(clang, *cflags, os.path.join(here, "kernel.c"), "-o", o("kernel.o"))
run(clang, *cflags, os.path.join(here, "system.c"), "-o", o("system.o"))
run(clang, *cflags, os.path.join(here, "libc.c"), "-o", o("libc.o"))
run(clang, *cflags, os.path.join(here, "ajm.c"), "-o", o("ajm.o"))
run(clang, *cflags, os.path.join(here, "videodec2.c"), "-o", o("videodec2.o"))
run(sys.executable, os.path.join(here, "h264.py"), o("h264_stream.c"))
run(clang, *cflags, o("h264_stream.c"), "-o", o("h264_stream.o"))
run(lld, "-pie", "--no-dynamic-linker", "--eh-frame-hdr", "-e", "_start", o("eboot.o"),
    o("kernel.o"), o("system.o"), o("libc.o"), o("ajm.o"), o("videodec2.o"),
    o("h264_stream.o"), o("libTest.so"), o("prx.so"),
    o("libkernel.so"), *[o(lib + ".so") for lib, _ in LIBS], "-o", o("eboot.so"))
run(sys.executable, sce, o("eboot.so"), o("eboot.elf"), "--name", "eboot",
    "--lib", "libTest=" + o("libTest.so"), "--lib", "libTestPrx=" + o("prx.so"),
    "--lib", "libkernel=" + o("libkernel.so"),
    *[a for lib, _ in LIBS for a in ("--lib", lib + "=" + o(lib + ".so"))])


os.makedirs(o("app0/sce_module"), exist_ok=True)
shutil.copyfile(o("eboot.elf"), o("app0/eboot.bin"))
shutil.copyfile(o("prx.elf"), o("app0/sce_module/libTestPrx.prx"))
shutil.copyfile(o("load.elf"), o("app0/sce_module/libTestLoad.prx"))
