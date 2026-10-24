#!/usr/bin/env python3
"""Rewrites an x86-64 ELF linked by ld.lld as an Orbis (PS4) one, for tests.

The input is a position-independent executable (output: ET_SCE_DYNEXEC) or,
with --prx, a shared object (ET_SCE_DYNAMIC). Its loadable segments, TLS
and .eh_frame_hdr are kept as they are; its dynamic symbols, relocations and
DT_INIT go into the SCE tables of a new PT_DYNAMIC and PT_SCE_DYNLIBDATA
(platforms/orbis/src/orbis/elf_image.h), with every symbol named
"NID#library#module". Imports belong to the library of the stub shared
object that defines them (--lib NAME=STUB.so); exports to the module itself
(--name NAME).

usage: orbis_sce_elf.py IN OUT --name NAME [--prx] [--lib NAME=STUB.so]...
"""
import argparse
import base64
import hashlib
import struct

NID_SUFFIX = bytes.fromhex("518d64a635ded8c1e6b039b1c3e55230")
ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"

PT_LOAD, PT_DYNAMIC, PT_TLS = 1, 2, 7
PT_GNU_EH_FRAME, PT_SCE_DYNLIBDATA = 0x6474E550, 0x61000000
DT_INIT, DT_SONAME = 0x0C, 0x0E
DT_SCE_MODULE_INFO, DT_SCE_NEEDED_MODULE = 0x6100000D, 0x6100000F
DT_SCE_EXPORT_LIB, DT_SCE_IMPORT_LIB = 0x61000013, 0x61000015
DT_SCE_JMPREL, DT_SCE_PLTREL, DT_SCE_PLTRELSZ = 0x61000029, 0x6100002B, 0x6100002D
DT_SCE_RELA, DT_SCE_RELASZ, DT_SCE_RELAENT = 0x6100002F, 0x61000031, 0x61000033
DT_SCE_STRTAB, DT_SCE_STRSZ = 0x61000035, 0x61000037
DT_SCE_SYMTAB, DT_SCE_SYMENT, DT_SCE_SYMTABSZ = 0x61000039, 0x6100003B, 0x6100003F


def nid(name):
    digest = hashlib.sha1(name.encode() + NID_SUFFIX).digest()[:8]
    return base64.b64encode(digest[::-1]).decode()[:11].replace("/", "-")


class Elf:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        assert d[:4] == b"\x7fELF" and d[4] == 2, path
        self.phoff, self.shoff = struct.unpack_from("<QQ", d, 0x20)
        self.phnum, = struct.unpack_from("<H", d, 0x38)
        self.shnum, self.shstrndx = struct.unpack_from("<HH", d, 0x3C)
        self.phs = [struct.unpack_from("<IIQQQQQQ", d, self.phoff + 56 * i)
                    for i in range(self.phnum)]
        self.secs = {}
        shs = [struct.unpack_from("<IIQQQQIIQQ", d, self.shoff + 64 * i)
               for i in range(self.shnum)]
        names = shs[self.shstrndx][4]
        for sh in shs:
            n = d[names + sh[0]:d.index(b"\0", names + sh[0])].decode()
            self.secs[n] = sh  # name, type, flags, addr, offset, size, ...
        self.dyn = {}
        for p in self.phs:
            if p[0] == PT_DYNAMIC:
                for o in range(p[2], p[2] + p[5], 16):
                    t, v = struct.unpack_from("<qQ", d, o)
                    if t == 0:
                        break
                    self.dyn.setdefault(t, v)

    def section(self, name):
        sh = self.secs.get(name)
        return self.d[sh[4]:sh[4] + sh[5]] if sh else b""

    def symbols(self):
        strtab = self.section(".dynstr")
        syms = self.section(".dynsym")
        out = []
        for o in range(0, len(syms), 24):
            st_name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", syms, o)
            out.append((strtab[st_name:strtab.index(b"\0", st_name)].decode(), info, other,
                        shndx, value, size))
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--name", required=True)
    ap.add_argument("--prx", action="store_true")
    ap.add_argument("--lib", action="append", default=[])
    a = ap.parse_args()

    elf = Elf(a.inp)
    owner = {}  # import name -> library
    libs = []
    for spec in a.lib:
        lib, stub = spec.split("=", 1)
        libs.append(lib)
        for name, info, _, shndx, _, _ in Elf(stub).symbols():
            if name and shndx:
                owner[name] = lib

    strtab = bytearray(b"\0")

    def string(s):
        off = len(strtab)
        strtab.extend(s.encode() + b"\0")
        return off

    lib_id = {lib: i + 1 for i, lib in enumerate(libs)}
    rela, jmprel = elf.section(".rela.dyn"), elf.section(".rela.plt")
    used = {struct.unpack_from("<Q", t, o + 8)[0] >> 32
            for t in (rela, jmprel) for o in range(0, len(t), 24)}
    symtab = bytearray()
    for i, (name, info, other, shndx, value, size) in enumerate(elf.symbols()):
        st_name = 0
        # Defined globals are exports; undefined ones imports, unless
        # nothing refers to them (a linked library's own imports).
        if name and info >> 4 and (shndx or i in used):
            if shndx:
                st_name = string(f"{nid(name)}#A#A")
            else:
                lib = owner.get(name)
                if lib is None:
                    raise SystemExit(f"{a.inp}: no --lib defines {name}")
                c = ALPHABET[lib_id[lib]]
                st_name = string(f"{nid(name)}#{c}#{c}")
        symtab += struct.pack("<IBBHQQ", st_name, info, other, shndx, value, size)
    tags = []
    if DT_INIT in elf.dyn:
        tags.append((DT_INIT, elf.dyn[DT_INIT]))
    self_name = string(a.name)
    tags += [(DT_SCE_MODULE_INFO, self_name), (DT_SCE_EXPORT_LIB, self_name)]
    for lib in libs:
        off = string(lib)
        tags += [(DT_SCE_NEEDED_MODULE, lib_id[lib] << 48 | off),
                 (DT_SCE_IMPORT_LIB, lib_id[lib] << 48 | off)]
    dynlib = bytearray(strtab)
    dynlib += b"\0" * (-len(dynlib) % 8)
    sym_off = len(dynlib)
    dynlib += symtab
    rela_off = len(dynlib)
    dynlib += rela
    jmprel_off = len(dynlib)
    dynlib += jmprel
    tags += [(DT_SCE_STRTAB, 0), (DT_SCE_STRSZ, len(strtab)),
             (DT_SCE_SYMTAB, sym_off), (DT_SCE_SYMENT, 24), (DT_SCE_SYMTABSZ, len(symtab)),
             (DT_SCE_RELA, rela_off), (DT_SCE_RELASZ, len(rela)), (DT_SCE_RELAENT, 24),
             (DT_SCE_JMPREL, jmprel_off), (DT_SCE_PLTRELSZ, len(jmprel)), (DT_SCE_PLTREL, 7),
             (0, 0)]
    dynamic = b"".join(struct.pack("<qQ", t, v) for t, v in tags)

    out = bytearray(elf.d)
    out += b"\0" * (-len(out) % 16)
    dynlib_at = len(out)
    out += dynlib
    out += b"\0" * (-len(out) % 16)
    dynamic_at = len(out)
    out += dynamic
    phs = [p for p in elf.phs if p[0] in (PT_LOAD, PT_TLS, PT_GNU_EH_FRAME)]
    phs.append((PT_DYNAMIC, 6, dynamic_at, 0, 0, len(dynamic), len(dynamic), 8))
    phs.append((PT_SCE_DYNLIBDATA, 4, dynlib_at, 0, 0, len(dynlib), 0, 16))
    out += b"\0" * (-len(out) % 8)
    phoff = len(out)
    for p in phs:
        out += struct.pack("<IIQQQQQQ", *p)
    struct.pack_into("<H", out, 16, 0xFE18 if a.prx else 0xFE10)
    struct.pack_into("<Q", out, 0x20, phoff)
    struct.pack_into("<Q", out, 0x28, 0)            # no section headers
    struct.pack_into("<HHHH", out, 0x38, len(phs), 64, 0, 0)
    open(a.out, "wb").write(out)


if __name__ == "__main__":
    main()
