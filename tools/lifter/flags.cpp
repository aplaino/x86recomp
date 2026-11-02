#include "flags.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>

namespace lifter {
namespace {

bool IsShiftOrRotate(ZydisMnemonic m) {
  switch (m) {
    case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR:
    case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_ROR: case ZYDIS_MNEMONIC_RCL:
    case ZYDIS_MNEMONIC_RCR: case ZYDIS_MNEMONIC_SHLD: case ZYDIS_MNEMONIC_SHRD:
      return true;
    default:
      return false;
  }
}

// The function writes fs:[0] (registers an SEH frame) or calls setjmp: it
// can be resumed into, and its activation returns whatever the
// continuation's return left in Ctx (see GuestResume).
bool Resumable(const Function& f) {
  if (!f.setjmp_returns.empty()) return true;
  for (const auto& [va, in] : f.insns) {
    if (!in.ok || in.d.mnemonic != ZYDIS_MNEMONIC_MOV) continue;
    const auto& o0 = in.ops[0];
    if (o0.type == ZYDIS_OPERAND_TYPE_MEMORY && o0.mem.segment == ZYDIS_REGISTER_FS &&
        o0.mem.base == ZYDIS_REGISTER_NONE && o0.mem.index == ZYDIS_REGISTER_NONE &&
        o0.mem.disp.value == 0)
      return true;
  }
  return false;
}

// call/jmp [slot] through an import's IAT slot: the slot, else 0.
uint64_t ImportSlot(const Program& p, const Insn& in) {
  const auto& o = in.ops[0];
  if (o.type != ZYDIS_OPERAND_TYPE_MEMORY || o.mem.base != ZYDIS_REGISTER_NONE ||
      o.mem.index != ZYDIS_REGISTER_NONE)
    return 0;
  const uint64_t slot = uint32_t(o.mem.disp.value);
  return p.iat.count(slot) ? slot : 0;
}

// One instruction of a function in the liveness graph.
enum class Kind : uint8_t {
  Normal,    // in = use | (out & ~kill)
  Call,      // direct call of fns[target]: in = reads(target)
  CallInd,   // indirect call: in = out (no callee reads flags at entry)
  CallImp,   // import call: in = use | out (use: all for flags_iat)
  Full,      // the full spill (resume sites, unknown targets): in = all
  Ret,       // in = ret(f) | indirect
  TailInd,   // indirect jump out: in = ret(f) | indirect
  TailImp,   // jump to an import: in = use | ret(f) | indirect
  Abort,     // fatal (BadJump, unimplemented): in = 0
};

struct Node {
  uint32_t use = 0, kill = 0;
  Kind kind = Kind::Normal;
  int32_t target = -1;              // Call: function index
  uint32_t succ_begin = 0, succ_count = 0;
};

// A function's graph. A successor >= 0 is a node; s < 0 is a tail call
// to function index -s - 1 (in = reads of it).
struct Graph {
  uint64_t entry = 0;
  bool resumable = false;
  std::vector<Node> nodes;
  std::vector<int32_t> succ;
  std::vector<uint32_t> live;  // live-in per node, from the last solve
  std::vector<uint64_t> vas;   // address per node
  int32_t entry_node = -1;
  std::vector<std::pair<uint64_t, int32_t>> hook_ends;  // hook va, node of its end
};

}  // namespace

FlagEffect InsnFlags(const Insn& in) {
  FlagEffect e;
  if (!in.ok || !in.d.cpu_flags) return e;
  const auto& f = *in.d.cpu_flags;
  const auto m = in.d.mnemonic;
  e.use = f.tested & kArithFlags;
  e.kill = (f.modified | f.set_0 | f.set_1) & ~f.undefined & kArithFlags;
  switch (m) {
    case ZYDIS_MNEMONIC_PUSHFD: case ZYDIS_MNEMONIC_PUSHF:
      e.use = kArithFlags;
      break;
    case ZYDIS_MNEMONIC_POPFD: case ZYDIS_MNEMONIC_POPF:
      e.kill = kArithFlags;
      break;
    case ZYDIS_MNEMONIC_LAHF:
      e.use = kArithFlags & ~0x800u;
      break;
    case ZYDIS_MNEMONIC_SAHF:
      e.kill = kArithFlags & ~0x800u;
      break;
    default:
      break;
  }
  // Flags x86 leaves undefined but the lifted code writes on every
  // execution (lifted.h: Logic clears AF; the shifts write AF and OF, the
  // rotates and double shifts OF, whenever the count is not 0). The other
  // undefined flags (MUL/IMUL's SF ZF AF PF, BSF/BSR's, DIV's, ...) are
  // left as they were by the lifted code, so they pass through.
  switch (m) {
    case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR:
    case ZYDIS_MNEMONIC_TEST:
      e.kill |= 0x10u;
      break;
    case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR:
      e.kill |= 0x810u & (f.modified | f.set_0 | f.set_1 | f.undefined);
      break;
    case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_ROR: case ZYDIS_MNEMONIC_SHLD:
    case ZYDIS_MNEMONIC_SHRD:
      e.kill |= 0x800u & (f.modified | f.set_0 | f.set_1 | f.undefined);
      break;
    default:
      break;
  }
  // A shift or rotate by a count that may be 0 (mod 32) leaves the flags
  // as they were.
  if (IsShiftOrRotate(m)) {
    const auto& cnt = in.ops[in.d.operand_count_visible - 1];
    if (in.d.operand_count_visible < 2 || cnt.type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
        (cnt.imm.value.u & 0x1F) == 0)
      e.kill = 0;
    // RCL/RCR rotate by the masked count mod 9 or 17 for bytes and
    // words: CF stays as it was when that is 0.
    if (m == ZYDIS_MNEMONIC_RCL || m == ZYDIS_MNEMONIC_RCR) e.kill = 0;
  }
  // rep cmps/scas with ecx = 0 compare nothing; the ZF they test is the
  // one each compare has just written.
  if (in.d.meta.category == ZYDIS_CATEGORY_STRINGOP &&
      (in.d.attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE)))
    e.use = e.kill = 0;
  return e;
}

void FlagLiveness::Run(const Program& p, const Decoder& dec, std::string* report,
                       const std::set<uint64_t>& dump) {
  p_ = &p;
  std::vector<uint64_t> entries(p.entries.begin(), p.entries.end());
  auto index = [&](uint64_t va) -> int32_t {
    auto it = std::lower_bound(entries.begin(), entries.end(), va);
    return it != entries.end() && *it == va ? int32_t(it - entries.begin()) : -1;
  };
  const uint32_t kAll = kArithFlags;
  std::vector<Graph> gs(entries.size());
  std::vector<std::vector<int32_t>> callers(entries.size());  // also tail callers
  size_t nodes = 0;
  // Entries whose address the program takes: those an indirect call or
  // jump can reach (a dword in .rdata/.data, a pointer the relocations
  // name elsewhere, an immediate in code). Functions entered only by
  // direct calls and jumps (or by the host: the EXE's entry, exports)
  // do not count towards `any`.
  std::vector<bool> taken(entries.size(), false);
  auto take = [&](uint64_t v) {
    if (const int32_t i = index(v); i >= 0) taken[i] = true;
  };
  for (const auto& m : p.modules) {
    for (const auto& sec : m.img.sections)
      if (sec.name == ".rdata" || sec.name == ".data")
        for (uint64_t a = sec.va; a + 4 <= sec.va + sec.vsize; a += 4)
          take(p.mem.read<uint32_t>(a));
    for (uint64_t at : m.img.fixups)
      if (p.mem.IsCommitted(at)) take(p.mem.read<uint32_t>(at));
  }

  for (size_t fi = 0; fi < entries.size(); fi++) {
    Function f;
    BuildFunction(p, dec, entries[fi], &f);
    Graph& g = gs[fi];
    g.entry = f.entry;
    g.resumable = Resumable(f);
    std::map<uint64_t, int32_t> at;
    for (const auto& [va, in] : f.insns) at.emplace(va, int32_t(at.size()));
    g.nodes.resize(at.size());
    for (const auto& [va, i] : at) g.vas.push_back(va);
    if (auto e = at.find(f.entry); e != at.end()) g.entry_node = e->second;
    // Manifest hooks: [va, end) runs the game's hook_<va> with the full
    // spill around it, then continues at end; labels inside the range
    // lead there too. A hook reads no guest flags (game hooks only set
    // them, in c.eflags, for the code after them), so flags pass through.
    std::map<uint64_t, uint64_t> hooks;
    for (const auto& [va, end] : p.hooks)
      if (f.insns.count(va) && f.insns.count(end)) {
        hooks[va] = end;
        g.hook_ends.emplace_back(va, at[end]);
      }

    for (const auto& [va, in] : f.insns) {
      Node& n = g.nodes[at.at(va)];
      n.succ_begin = uint32_t(g.succ.size());
      auto add_succ = [&](uint64_t t) {
        if (IsOtherEntry(p, f, t)) {
          const int32_t ti = index(t);
          if (ti < 0) {
            n.kind = Kind::Full;
            return;
          }
          g.succ.push_back(-ti - 1);
          callers[ti].push_back(int32_t(fi));
        } else if (auto s = at.find(t); s != at.end()) {
          g.succ.push_back(s->second);
        }
        // else: BadJump, fatal
      };
      auto finish = [&]() { n.succ_count = uint32_t(g.succ.size()) - n.succ_begin; };

      auto h = hooks.upper_bound(va);
      if (h != hooks.begin() && va < std::prev(h)->second) {  // in a hook's range
        add_succ(std::prev(h)->second);
        finish();
        continue;
      }
      if (!in.ok) {
        n.kind = Kind::Abort;
        finish();
        continue;
      }
      const FlagEffect fe = InsnFlags(in);
      n.use = fe.use;
      n.kill = fe.kill;
      for (int i = 0; i < in.d.operand_count_visible; i++)
        if (in.ops[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && !in.ops[i].imm.is_relative)
          take(uint32_t(in.ops[i].imm.value.u));
      const auto m = in.d.mnemonic;
      const auto& op0 = in.ops[0];
      if ((m == ZYDIS_MNEMONIC_CALL || m == ZYDIS_MNEMONIC_JMP) && p.resume_sites.count(va)) {
        n.kind = Kind::Full;  // throws GuestResume after the full spill
        finish();
        continue;
      }
      uint64_t t;
      const Flow fl = Classify(p, f, in, &t);
      switch (m) {
        case ZYDIS_MNEMONIC_INT: case ZYDIS_MNEMONIC_INT3: case ZYDIS_MNEMONIC_INT1:
        case ZYDIS_MNEMONIC_INTO: case ZYDIS_MNEMONIC_HLT: case ZYDIS_MNEMONIC_UD2:
          n.kind = Kind::Abort;  // Trap: fatal (runtime/dispatch.cpp)
          break;
        default:
          break;
      }
      if (m == ZYDIS_MNEMONIC_CALL) {
        if (op0.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
          const uint64_t ct = RelTarget(in);
          if (ct == in.next()) {
            // call $+5: a push of EIP
          } else if (!p.InText(ct)) {
            n.kind = Kind::Abort;
          } else if (const int32_t ti = index(ct); ti < 0) {
            n.kind = Kind::Full;
          } else {
            n.kind = Kind::Call;
            n.target = ti;
            callers[ti].push_back(int32_t(fi));
          }
        } else if (const uint64_t slot = ImportSlot(p, in)) {
          n.kind = Kind::CallImp;
          n.use = p.flags_iat.count(slot) ? kAll : 0;
        } else {
          n.kind = Kind::CallInd;
        }
      }
      switch (fl) {
        case Flow::Fall:
        case Flow::Branch:
          if (fl == Flow::Branch) add_succ(t);
          add_succ(in.next());
          break;
        case Flow::Jump:
        case Flow::TailCall:
          add_succ(t);
          break;
        case Flow::JumpTable:
          for (uint64_t jt : p.jumptables.at(va)) add_succ(jt);
          break;
        case Flow::TailDispatch:
          if (const uint64_t slot = ImportSlot(p, in)) {
            n.kind = Kind::TailImp;
            n.use = p.flags_iat.count(slot) ? kAll : 0;
          } else {
            n.kind = Kind::TailInd;
          }
          break;
        case Flow::Return:
          n.kind = Kind::Ret;
          break;
        case Flow::Stop:  // a no-return call, or a trap
          break;
      }
      finish();
    }
    g.live.assign(g.nodes.size(), 0);
    nodes += g.nodes.size();
  }

  // Least fixpoint over the program: masks only grow.
  // A function the game replaces (manifest `replace`) counts as its lifted
  // body: the replacement reads no guest flags and either calls the body
  // or leaves c.eflags as the caller spilled it.
  std::vector<Fn> fn(entries.size());
  uint32_t any = 0, indirect = 0;
  char buf0[160];
  std::string seeds;  // where `any` and `indirect` got each new flag
  // Where each function's ret first grew: the caller and its site.
  std::vector<std::pair<int32_t, uint64_t>> ret_from(entries.size(), {-1, 0});
  int32_t first_indirect = -1;
  auto seed = [&](const char* what, uint32_t bits, uint64_t f, uint64_t va) {
    char b[128];
    std::snprintf(b, sizeof b, "  %s %03X from %08llX at %08llX\n", what, bits,
                  (unsigned long long)f, (unsigned long long)va);
    seeds += b;
  };
  std::deque<int32_t> work;
  std::vector<bool> queued(entries.size(), true);
  for (size_t i = 0; i < entries.size(); i++) work.push_back(int32_t(i));
  size_t solves = 0;
  auto enqueue = [&](int32_t i) {
    if (!queued[i]) {
      queued[i] = true;
      work.push_back(i);
    }
  };
  while (!work.empty()) {
    const int32_t fi = work.front();
    work.pop_front();
    queued[fi] = false;
    Graph& g = gs[fi];
    solves++;
    const uint32_t ret = fn[fi].ret | indirect;
    // Backward over the nodes (address order) until nothing changes.
    for (bool changed = true; changed;) {
      changed = false;
      for (size_t k = g.nodes.size(); k-- > 0;) {
        const Node& n = g.nodes[k];
        uint32_t out = 0;
        for (uint32_t s = 0; s < n.succ_count; s++) {
          const int32_t v = g.succ[n.succ_begin + s];
          out |= v >= 0 ? g.live[v] : fn[-v - 1].reads;
        }
        uint32_t in = 0;
        switch (n.kind) {
          case Kind::Normal: in = n.use | (out & ~n.kill); break;
          case Kind::Call: in = fn[n.target].reads; break;
          case Kind::CallInd: in = out; break;
          case Kind::CallImp: in = n.use | out; break;
          case Kind::Full: in = kAll; break;
          case Kind::Ret: in = ret; break;
          case Kind::TailInd: in = ret; break;
          case Kind::TailImp: in = n.use | ret; break;
          case Kind::Abort: in = 0; break;
        }
        if (in != g.live[k]) {
          g.live[k] = in;
          changed = true;
        }
      }
    }
    // What this function tells the others. A weak entry (found only by a
    // pointer scan: Program::weak; often data that decodes as code) tells
    // nothing: its own spills follow from its body, but what it reads after
    // a call or an indirect jump puts no constraint on other functions.
    // When one runs, --flags-check sees such a read.
    const bool weak = p.weak.count(g.entry);
    const uint32_t reads = g.entry_node >= 0 ? g.live[g.entry_node] : kAll;
    uint32_t new_any = any, new_indirect = indirect;
    if (reads & ~fn[fi].reads) {
      fn[fi].reads |= reads;
      if (taken[fi]) {
        if (reads & ~new_any) seed("any", reads & ~new_any, g.entry, g.entry);
        new_any |= reads;
      }
      for (int32_t c : callers[fi]) enqueue(c);
    }
    for (size_t k = 0; k < g.nodes.size(); k++) {
      const Node& n = g.nodes[k];
      uint32_t out = 0;
      for (uint32_t s = 0; s < n.succ_count; s++) {
        const int32_t v = g.succ[n.succ_begin + s];
        out |= v >= 0 ? g.live[v] : fn[-v - 1].reads;
        if (v < 0 && !weak && (ret & ~fn[-v - 1].ret)) {  // tail call: it returns for us
          if (ret_from[-v - 1].first < 0) ret_from[-v - 1] = {fi, g.vas[k]};
          fn[-v - 1].ret |= ret;
          enqueue(-v - 1);
        }
      }
      if (weak) continue;
      if (n.kind == Kind::Call && (out & ~fn[n.target].ret)) {
        if (ret_from[n.target].first < 0) ret_from[n.target] = {fi, g.vas[k]};
        fn[n.target].ret |= out;
        enqueue(n.target);
      }
      const uint32_t ind = n.kind == Kind::CallInd ? out : n.kind == Kind::TailInd ? ret : 0;
      if (ind & ~new_indirect) {
        seed("indirect", ind & ~new_indirect, g.entry, g.vas[k]);
        if (first_indirect < 0) first_indirect = fi;
      }
      new_indirect |= ind;
    }
    if (g.resumable && !weak) {
      if (ret & ~new_indirect) seed("indirect (resumable)", ret & ~new_indirect, g.entry, g.entry);
      new_indirect |= ret;
    }
    any = new_any;  // reported only: see flags.h
    if (new_indirect != indirect) {  // every return keeps it
      indirect = new_indirect;
      for (size_t i = 0; i < entries.size(); i++) enqueue(int32_t(i));
    }
  }

  fns_.clear();
  sites_.clear();
  for (const Graph& g : gs) {
    if (!dump.count(g.entry)) continue;
    std::string& d = dumps_[g.entry];
    static const char* kKind[] = {"",    "call", "call*",      "import", "full",
                                  "ret", "jmp*", "jmp import", "abort"};
    for (size_t k = 0; k < g.nodes.size(); k++) {
      const Node& n = g.nodes[k];
      std::snprintf(buf0, sizeof buf0, "  %08llX live %03X use %03X kill %03X %s\n",
                    (unsigned long long)g.vas[k], g.live[k], n.use, n.kill, kKind[int(n.kind)]);
      d += buf0;
    }
  }
  for (const Graph& g : gs)
    for (size_t k = 0; k < g.nodes.size(); k++)
      if (g.nodes[k].kind == Kind::CallInd || g.nodes[k].kind == Kind::CallImp)
        sites_[g.entry << 32 | g.vas[k]] = g.live[k];
  size_t n_reads = 0, n_ret = 0;
  std::string list;
  for (size_t i = 0; i < entries.size(); i++) {
    fns_[entries[i]] = fn[i];
    if (fn[i].reads) {
      n_reads++;
      char buf[64];
      std::snprintf(buf, sizeof buf, "  %08llX reads %03X ret %03X%s\n",
                    (unsigned long long)entries[i], fn[i].reads, fn[i].ret,
                    taken[i] ? " (address taken)" : "");
      list += buf;
    } else if (fn[i].ret) {
      n_ret++;
    }
  }
  any_ = any;
  indirect_ = indirect;
  done_ = true;
  char buf[256];
  std::snprintf(buf, sizeof buf,
                "flags: %zu functions, %zu instructions, %zu solves; any %03X, indirect %03X; "
                "%zu read flags at entry, %zu more return them\n",
                entries.size(), nodes, solves, any, indirect, n_reads, n_ret);
  // Why the first function that fed `indirect` returns flags: who first
  // read them after it, and why that one returns them, and so on.
  for (int32_t i = first_indirect, steps = 0; i >= 0 && steps < 12; steps++) {
    const auto [c, va] = ret_from[i];
    if (c < 0) break;
    std::snprintf(buf0, sizeof buf0, "  ret of %08llX from %08llX at %08llX\n",
                  (unsigned long long)entries[i], (unsigned long long)entries[c],
                  (unsigned long long)va);
    seeds += buf0;
    i = c;
  }
  *report += buf + seeds + list;
  // Flags read after a manifest hook before being written. The full spill
  // before the hook keeps them, unless the hook's C++ calls a lifted
  // function (whose returns keep only what lifted callers read): then it
  // must set them itself. Listed for review.
  for (const Graph& g : gs)
    for (const auto& [hva, end] : g.hook_ends)
      if (g.live[end]) {
        std::snprintf(buf, sizeof buf, "  hook %08llX in %08llX: flags %03X live after it\n",
                      (unsigned long long)hva, (unsigned long long)g.entry, g.live[end]);
        *report += buf;
      }
}

uint32_t FlagLiveness::Reads(uint64_t t) const {
  if (!done_) return kArithFlags;
  auto it = fns_.find(t);
  return it == fns_.end() ? kArithFlags : it->second.reads;
}

std::string FlagLiveness::Dump(uint64_t f) const {
  auto it = dumps_.find(f);
  return it == dumps_.end() ? std::string() : it->second;
}

uint32_t FlagLiveness::CallSite(uint64_t f, uint64_t va) const {
  if (!done_) return kArithFlags;
  auto it = sites_.find(f << 32 | va);
  return it == sites_.end() ? kArithFlags : it->second;
}

uint32_t FlagLiveness::Ret(uint64_t f) const {
  if (!done_) return kArithFlags;
  auto it = fns_.find(f);
  return (it == fns_.end() ? kArithFlags : it->second.ret) | indirect_;
}

}  // namespace lifter
