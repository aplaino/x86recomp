// Direct3D 11's shader programs (d3d11_impl.h, "shader programs"): those
// d3d11_link.cpp links, kept per combination of shaders and input layout.
// Draws look them up by the objects' serials; a combination not seen
// yet is looked up by content (the shaders' DXBC hashes and the layout's
// elements), so objects a game makes again from the same bytecode share
// one program.

#include "win32/d3d11_impl.h"

namespace recomp::hle::d3d11 {

namespace {

uint64_t Mix(uint64_t h, uint64_t v) {
  for (int i = 0; i < 8; i++) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 0x100000001B3ull;
  return h;
}

uint64_t LayoutHash(const InputLayout* l) {
  if (!l) return 0;
  uint64_t h = 0xCBF29CE484222325ull;
  for (const InputLayout::Element& e : l->elements) {
    for (char c : e.semantic) h = Mix(h, uint8_t(c));
    for (uint32_t v : {e.index, e.format, e.slot, e.offset, e.per_instance, e.step_rate}) h = Mix(h, v);
  }
  return h | 1;
}

uint64_t ShaderKey(const Shader* s) { return s ? Mix(s->hash, s->dxbc.size()) : 0; }

}  // namespace

std::shared_ptr<const LinkedProgram> ProgramFor(Device& dev, const Shader& vs, const Shader* ps,
                                                const InputLayout* layout) {
  const auto key = std::make_tuple(vs.serial, ps ? ps->serial : 0, layout ? layout->serial : 0);
  auto it = dev.programs.find(key);
  if (it != dev.programs.end()) return it->second;
  const auto content = std::make_tuple(ShaderKey(&vs), ShaderKey(ps), LayoutHash(layout));
  auto jt = dev.linked.find(content);
  std::shared_ptr<const LinkedProgram> p;
  if (jt != dev.linked.end()) {
    p = jt->second;
  } else {
    p = LinkProgram(*dev.gfx, vs, ps, layout);
    dev.linked.emplace(content, p);
    if (p) dev.stats.linked++;
  }
  dev.programs.emplace(key, p);
  return p;
}

std::shared_ptr<const LinkedProgram> PointPatchProgramFor(Device& dev, const Shader& vs, const Shader& hs,
                                                          const Shader& ds, const Shader* ps,
                                                          const InputLayout* layout) {
  // By the objects' serials only: point patch programs are few.
  const auto key = std::make_tuple(vs.serial, hs.serial, ds.serial, ps ? ps->serial : 0,
                                   layout ? layout->serial : 0);
  auto it = dev.point_patch_programs.find(key);
  if (it != dev.point_patch_programs.end()) return it->second;
  auto p = LinkPointPatchProgram(*dev.gfx, vs, hs, ds, ps, layout);
  if (p) dev.stats.linked++;
  dev.point_patch_programs.emplace(key, p);
  return p;
}

std::shared_ptr<const LinkedProgram> GeometryProgramFor(Device& dev, const Shader& vs, const Shader& gs,
                                                        const Shader* ps, const InputLayout* layout) {
  // By the objects' serials only, as point patches'.
  const auto key = std::make_tuple(vs.serial, gs.serial, ps ? ps->serial : 0, layout ? layout->serial : 0);
  auto it = dev.geometry_programs.find(key);
  if (it != dev.geometry_programs.end()) return it->second;
  auto p = LinkGeometryProgram(*dev.gfx, vs, gs, ps, layout);
  if (p) dev.stats.linked++;
  dev.geometry_programs.emplace(key, p);
  return p;
}

std::shared_ptr<const LinkedProgram> ComputeProgramFor(Device& dev, const Shader& cs) {
  const auto key = std::make_tuple(cs.serial, uint64_t(0), uint64_t(0));
  auto it = dev.programs.find(key);
  if (it != dev.programs.end()) return it->second;
  const auto content = std::make_tuple(ShaderKey(&cs), uint64_t(0), uint64_t(0));
  auto jt = dev.linked.find(content);
  std::shared_ptr<const LinkedProgram> p;
  if (jt != dev.linked.end()) {
    p = jt->second;
  } else {
    p = LinkComputeProgram(*dev.gfx, cs);
    dev.linked.emplace(content, p);
    if (p) dev.stats.linked++;
  }
  dev.programs.emplace(key, p);
  return p;
}

}  // namespace recomp::hle::d3d11
