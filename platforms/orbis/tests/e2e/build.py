#!/usr/bin/env python3
"""Builds orbis_process_test's eboot.elf and prx.elf into OUTDIR.

usage: build.py OUTDIR CLANG LD_LLD
"""
import os
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

run(clang, *cflags, os.path.join(here, "prx.c"), "-o", o("prx.o"))
run(lld, "-shared", "-soname", "libTestPrx", "--eh-frame-hdr", o("prx.o"), o("libkernel.so"),
    "-o", o("prx.so"))
run(sys.executable, sce, o("prx.so"), o("prx.elf"), "--prx", "--name", "libTestPrx",
    "--lib", "libkernel=" + o("libkernel.so"))

run(clang, *cflags, os.path.join(here, "eboot.c"), "-o", o("eboot.o"))
run(lld, "-pie", "--no-dynamic-linker", "--eh-frame-hdr", "-e", "_start", o("eboot.o"),
    o("libTest.so"), o("prx.so"), "-o", o("eboot.so"))
run(sys.executable, sce, o("eboot.so"), o("eboot.elf"), "--name", "eboot",
    "--lib", "libTest=" + o("libTest.so"), "--lib", "libTestPrx=" + o("prx.so"))
