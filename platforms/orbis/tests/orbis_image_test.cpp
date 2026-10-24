// orbis/elf_image and orbis/nid against a small ET_SCE_DYNEXEC built
// here: .eh_frame FDEs (one of discarded code), one import with its PLT
// stub, one export, and RELATIVE, R_X86_64_64 and JUMP_SLOT relocations.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "orbis/elf_image.h"
#include "orbis/nid.h"
#include "runtime/pe_image.h"

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    failures++;
  }
}

struct Buf {
  std::vector<uint8_t> d;
  template <typename T>
  void Put(size_t off, T v) {
    if (d.size() < off + sizeof(T)) d.resize(off + sizeof(T));
    std::memcpy(d.data() + off, &v, sizeof(T));
  }
  void Str(size_t off, const std::string& s) {
    if (d.size() < off + s.size() + 1) d.resize(off + s.size() + 1);
    std::memcpy(d.data() + off, s.c_str(), s.size() + 1);
  }
};

// File layout: headers at 0, text (vaddr 0) at 0x1000, data (vaddr
// 0x4000) at 0x2000, PT_DYNAMIC at 0x3000, dynlib data at 0x3400.
std::vector<uint8_t> BuildExec(const std::string& export_nid) {
  Buf b;
  b.d.assign(0x3800, 0);
  std::memcpy(b.d.data(), "\x7F" "ELF\x02\x01\x01", 7);
  b.Put<uint16_t>(16, 0xFE10);  // ET_SCE_DYNEXEC
  b.Put<uint16_t>(18, 62);
  b.Put<uint64_t>(24, 0x10);  // e_entry
  b.Put<uint64_t>(32, 64);    // e_phoff
  b.Put<uint16_t>(54, 56);
  b.Put<uint16_t>(56, 6);
  auto ph = [&](int i, uint32_t type, uint32_t flags, uint64_t off, uint64_t va, uint64_t filesz,
                uint64_t memsz) {
    const size_t o = 64 + i * 56;
    b.Put<uint32_t>(o, type);
    b.Put<uint32_t>(o + 4, flags);
    b.Put<uint64_t>(o + 8, off);
    b.Put<uint64_t>(o + 16, va);
    b.Put<uint64_t>(o + 32, filesz);
    b.Put<uint64_t>(o + 40, memsz);
  };
  ph(0, 1, 5, 0x1000, 0, 0x1000, 0x1000);              // text
  ph(1, 1, 6, 0x2000, 0x4000, 0x100, 0x2000);          // data + bss
  ph(2, 2, 6, 0x3000, 0, 0x200, 0x200);                // PT_DYNAMIC
  ph(3, 0x61000000, 4, 0x3400, 0, 0x400, 0);           // PT_SCE_DYNLIBDATA
  ph(4, 0x6474E550, 4, 0x1800, 0x800, 8, 8);           // PT_GNU_EH_FRAME
  ph(5, 7, 4, 0x2080, 0x4080, 0x10, 0x40);             // PT_TLS

  // .eh_frame_hdr at 0x800: version 1, eh_frame_ptr pcrel sdata4, no
  // count or table.
  const size_t text = 0x1000;
  b.d[text + 0x800] = 1;
  b.d[text + 0x801] = 0x1B;
  b.d[text + 0x802] = 0xFF;
  b.d[text + 0x803] = 0xFF;
  b.Put<int32_t>(text + 0x804, 0x900 - 0x804);
  // CIE at 0x900: "zR", code align 1, data align -8, ra 16, R = pcrel sdata4.
  const uint8_t cie[] = {0x10, 0, 0, 0, 0, 0, 0, 0, 1, 'z', 'R', 0, 1, 0x78, 16, 1, 0x1B, 0, 0, 0};
  std::memcpy(b.d.data() + text + 0x900, cie, sizeof cie);
  auto fde = [&](size_t at, uint32_t begin, uint32_t range) {
    b.Put<uint32_t>(text + at, 16);
    b.Put<uint32_t>(text + at + 4, uint32_t(at + 4 - 0x900));
    b.Put<int32_t>(text + at + 8, int32_t(begin) - int32_t(at + 8));
    b.Put<uint32_t>(text + at + 12, range);
    b.d[text + at + 16] = 0;  // augmentation data length
  };
  fde(0x914, 0x10, 0x20);
  fde(0x928, 0x40, 0x10);
  fde(0x93C, 0x944, 0x10);           // pc_begin 0: discarded code
  b.Put<uint32_t>(text + 0x950, 0);  // terminator
  // A PLT stub at 0x60 for the import's slot at 0x4000, which lazy
  // binding left pointing at the stub's push.
  const uint8_t stub[] = {0xFF, 0x25, 0, 0, 0, 0, 0x68, 0, 0, 0, 0};
  std::memcpy(b.d.data() + text + 0x60, stub, sizeof stub);
  b.Put<int32_t>(text + 0x62, 0x4000 - 0x66);
  b.Put<uint64_t>(0x2000, 0x66);

  // Dynlib data: strtab at 0, symtab at 0x100, rela at 0x200, jmprel at 0x300.
  const size_t lib = 0x3400;
  b.Str(lib + 1, "libc");
  b.Str(lib + 6, "libGame");
  b.Str(lib + 14, "game");
  b.Str(lib + 20, recomp::orbis::Nid("printf") + "#B#B");
  b.Str(lib + 40, export_nid + "#A#A");
  auto sym = [&](int i, uint32_t name, uint8_t info, uint16_t shndx, uint64_t value) {
    const size_t o = lib + 0x100 + i * 24;
    b.Put<uint32_t>(o, name);
    b.d[o + 4] = info;
    b.Put<uint16_t>(o + 6, shndx);
    b.Put<uint64_t>(o + 8, value);
  };
  sym(1, 20, 0x12, 0, 0);     // global function, imported
  sym(2, 40, 0x12, 1, 0x40);  // global function, exported
  auto rela = [&](size_t o, uint64_t off, uint32_t type, uint32_t s, int64_t addend) {
    b.Put<uint64_t>(lib + o, off);
    b.Put<uint64_t>(lib + o + 8, uint64_t(s) << 32 | type);
    b.Put<int64_t>(lib + o + 16, addend);
  };
  rela(0x200, 0x4008, 8, 0, 0x10);  // RELATIVE
  rela(0x218, 0x4010, 1, 2, 4);     // R_X86_64_64 to the export + 4
  rela(0x300, 0x4000, 7, 1, 0);     // JUMP_SLOT to the import

  size_t t = 0x3000;
  auto tag = [&](int64_t k, uint64_t v) {
    b.Put<int64_t>(t, k);
    b.Put<uint64_t>(t + 8, v);
    t += 16;
  };
  tag(0x0C, 0x10);                         // DT_INIT
  tag(0x61000035, 0), tag(0x61000037, 0x100);    // strtab
  tag(0x61000039, 0x100), tag(0x6100003F, 72);   // symtab: 3 entries
  tag(0x6100002F, 0x200), tag(0x61000031, 48);   // rela
  tag(0x61000029, 0x300), tag(0x6100002D, 24);   // jmprel
  tag(0x6100000F, uint64_t(1) << 48 | 1);        // needed module 1: libc
  tag(0x61000015, uint64_t(1) << 48 | 1);        // import lib 1: libc
  tag(0x6100000D, 14);                           // module info 0: game
  tag(0x61000013, 6);                            // export lib 0: libGame
  return b.d;
}

}  // namespace

int main() {
  using recomp::orbis::ElfImage;
  using recomp::orbis::Nid;
  Check(Nid("printf") == "hcuQgD53UxM", "Nid(printf)");

  const std::string export_nid = Nid("game_export");
  const auto dir = std::filesystem::temp_directory_path() / "orbis_image_test";
  std::filesystem::create_directories(dir);
  const std::string exe = (dir / "exec.elf").string();
  {
    std::ofstream out(exe, std::ios::binary);
    const auto d = BuildExec(export_nid);
    out.write(reinterpret_cast<const char*>(d.data()), d.size());
  }

  recomp::GuestMemory mem;
  if (!mem.Reserve(36)) {
    std::printf("cannot reserve the arena\n");
    return 1;
  }
  Check(ElfImage::IsElfFile(exe), "IsElfFile");
  ElfImage img;
  std::string err;
  if (!img.Load(exe, mem, &err)) {
    std::printf("FAIL: Load: %s\n", err.c_str());
    return 1;
  }
  const uint64_t b = recomp::orbis::kExecBase;
  Check(img.image_base == b && img.link_base == 0, "mapped at kExecBase");
  Check(img.entry == b + 0x10 && img.init == b + 0x10 && img.fini == 0, "entry, DT_INIT");
  Check(img.segments.size() == 2 && img.segments[1].va == b + 0x4000 &&
            img.segments[1].size == 0x2000, "segments");
  Check(mem.read<uint64_t>(b + 0x5F00) == 0 && mem.IsCommitted(b + 0x5F00), "bss is zero");

  Check(img.imports.size() == 1, "one import");
  if (img.imports.size() == 1) {
    const auto& imp = img.imports[0];
    Check(imp.nid == "hcuQgD53UxM" && imp.library == "libc" && imp.module == "libc" &&
              imp.type == 2, "import names");
    Check(imp.value == recomp::kThunkBase64, "import bound to the first thunk");
    Check(imp.slots.size() == 1 && imp.slots[0] == b + 0x4000, "import slot");
    Check(imp.plt == b + 0x60, "PLT stub");
  }
  Check(mem.read<uint64_t>(b + 0x4000) == recomp::kThunkBase64, "JUMP_SLOT");
  Check(mem.read<uint64_t>(b + 0x4008) == b + 0x10, "RELATIVE");
  Check(mem.read<uint64_t>(b + 0x4010) == b + 0x44, "R_X86_64_64 with addend");

  Check(img.exports.size() == 1, "one export");
  if (img.exports.size() == 1)
    Check(img.exports[0].nid == export_nid && img.exports[0].library == "libGame" &&
              img.exports[0].module == "game" && img.exports[0].va == b + 0x40, "export");

  Check(img.functions.size() == 2, "two FDEs");
  if (img.functions.size() == 2)
    Check(img.functions[0].begin == b + 0x10 && img.functions[0].end == b + 0x30 &&
              img.functions[1].begin == b + 0x40 && img.functions[1].end == b + 0x50,
          "FDE ranges");
  Check(img.has_tls && img.tls.init_va == b + 0x4080 && img.tls.init_size == 0x10 &&
            img.tls.mem_size == 0x40, "PT_TLS");

  // The same range again is refused; elsewhere it maps.
  ElfImage again;
  Check(!again.Load(exe, mem, &err), "overlap refused");
  Check(again.Load(exe, mem, &err, 0x10000000) && again.entry == 0x10000010, "moved");

  std::filesystem::remove_all(dir);
  std::printf("%s\n", failures ? "orbis_image: FAILED" : "orbis_image: ok");
  return failures ? 1 : 0;
}
