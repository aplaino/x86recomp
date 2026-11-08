// xboxkrnl: structured exception handling (docs/xboxkrnl.md, "Exceptions")
// and bug checks. Exceptions are dispatched as Windows NT does on its RISC
// processors: a first pass over the guest stack's frames calls each
// handler a function's .pdata entry names until one takes the exception;
// an unwind (RtlUnwind) is a second pass up to its target frame, which then
// continues at the target address. The frames come from the lifter's
// descriptions of the title's prologues (ppc/unwind.h); a continuation in
// another function's frame is a GuestResume, which that lifted function
// catches.

#include <cstring>
#include <initializer_list>
#include <vector>

#include "ppc/dispatch.h"
#include "ppc/fatal.h"
#include "ppc/unwind.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

using ppc::Ctx;
using ppc::Fatal;

constexpr const char* kKrnl = "xboxkrnl.exe";

// CONTEXT, 0xA40 bytes: the offsets titles' own C runtime reads (it
// restores CTR, the GPRs, CR, XER, FPSCR as a double and the FPRs from one,
// writes Iar at 8 and copies 0xA40 bytes). The others are chosen (console
// check): MSR at 4, LR at 0xC, VSCR in the last word of the quadword at
// 0x230, the 128 vector registers from 0x240 to the end.
constexpr uint32_t kContextSize = 0xA40, kCtxFlags = 0x0, kCtxMsr = 0x4, kCtxIar = 0x8,
                   kCtxLr = 0xC, kCtxCtr = 0x10, kCtxGpr = 0x18, kCtxCr = 0x118, kCtxXer = 0x11C,
                   kCtxFpscr = 0x120, kCtxFpr = 0x128, kCtxVscr = 0x230, kCtxVr = 0x240;
// ContextFlags: Windows NT's CONTEXT_FULL for the PowerPC (control,
// floating point, integer); the console's value is a console check.
constexpr uint32_t kContextFull = 0x7;
// EXCEPTION_RECORD: code, flags, a nested record, the address, the count
// of parameters and 15 of them.
constexpr uint32_t kRecordSize = 0x50, kRecFlags = 0x4, kRecAddress = 0xC, kRecParams = 0x10;
// DISPATCHER_CONTEXT, Windows NT's for the PowerPC: ControlPc,
// FunctionEntry, EstablisherFrame, ContextRecord (titles' frame handler
// reads the first three).
constexpr uint32_t kDcSize = 0x10;
constexpr uint32_t kNoncontinuable = 0x1, kUnwinding = 0x2, kExitUnwind = 0x4, kNestedCall = 0x10,
                   kTargetUnwind = 0x20;
constexpr uint32_t kContinueExecution = 0, kContinueSearch = 1;
constexpr uint32_t kStatusUnwind = 0xC0000027;
// A .pdata entry's bits 30 and 31: a function with an exception handler,
// whose address and data are the two words before the function.
constexpr uint32_t kPdataHandler = 0xC0000000;
// The return address handlers are called with: a walk that reaches it has
// left the handler's frames for the dispatcher that called it.
constexpr uint32_t kHandlerReturn = 0;

// A point of the walk: the registers inside a frame, at the call
// instruction `pc` (its return address less 4).
struct Point {
  Ctx s;
  uint32_t pc;
};
Point At(const Ctx& c) { return {c, uint32_t(c.lr) - 4}; }

// A dispatch or unwind in progress on this thread: where it started (the
// state at the call into the kernel), whether it is an unwind, and for a
// dispatch the frame whose handler it is calling.
struct Active {
  Point start;
  bool unwind;
  uint32_t target_ip;  // an unwind's
  uint32_t frame = 0;
};
thread_local std::vector<Active*> t_active;
struct Push {
  explicit Push(Active* a) { t_active.push_back(a); }
  ~Push() { t_active.pop_back(); }
};

// Guest memory below r1 for a dispatch's structures; handlers it calls run
// below it.
uint32_t Scratch(const Ctx& c, uint32_t size) {
  uint32_t limit, base;
  GuestStackRange(limit, base);
  uint32_t at = (uint32_t(c.r[1]) - 0x100 - size) & ~0xFu;
  if (at < limit + 0x1000 || at > base) Fatal(&c, "guest stack exhausted dispatching an exception");
  return at;
}

void WriteContext(uint32_t ctx, const Ctx& s, uint32_t iar) {
  std::memset(Mem() + ctx, 0, kContextSize);
  Wr32(ctx + kCtxFlags, kContextFull);
  Wr32(ctx + kCtxMsr, uint32_t(s.msr));
  Wr32(ctx + kCtxIar, iar);
  Wr32(ctx + kCtxLr, uint32_t(s.lr));
  Wr64(ctx + kCtxCtr, s.ctr);
  for (int i = 0; i < 32; i++) Wr64(ctx + kCtxGpr + 8 * i, s.r[i]);
  Wr32(ctx + kCtxCr, s.cr);
  Wr32(ctx + kCtxXer, s.xer);
  Wr64(ctx + kCtxFpscr, s.fpscr);
  for (int i = 0; i < 32; i++) Wr64(ctx + kCtxFpr + 8 * i, s.f[i]);
  Wr32(ctx + kCtxVscr + 12, s.vscr);
  for (int i = 0; i < 128; i++) std::memcpy(Mem() + ctx + kCtxVr + 16 * i, s.v[i].b, 16);
}

// The registers of a CONTEXT into s; returns its Iar.
uint32_t ReadContext(uint32_t ctx, Ctx& s) {
  s.msr = Rd32(ctx + kCtxMsr);
  s.lr = Rd32(ctx + kCtxLr);
  s.ctr = Rd64(ctx + kCtxCtr);
  for (int i = 0; i < 32; i++) s.r[i] = Rd64(ctx + kCtxGpr + 8 * i);
  s.cr = Rd32(ctx + kCtxCr);
  s.xer = Rd32(ctx + kCtxXer);
  s.fpscr = uint32_t(Rd64(ctx + kCtxFpscr));
  for (int i = 0; i < 32; i++) s.f[i] = Rd64(ctx + kCtxFpr + 8 * i);
  s.vscr = Rd32(ctx + kCtxVscr + 12);
  for (int i = 0; i < 128; i++) std::memcpy(s.v[i].b, Mem() + ctx + kCtxVr + 16 * i, 16);
  return Rd32(ctx + kCtxIar);
}

// Calls guest fn (a handler, a filter, a __finally block) below `below`,
// with its arguments in r3.., r12 = `r12` and the return address
// kHandlerReturn; the thread's registers are as they were after it.
uint32_t CallHandler(Ctx& c, uint32_t below, uint32_t fn, std::initializer_list<uint32_t> args,
                     uint32_t r12) {
  const Ctx saved = c;
  c.r[1] = (below - 0x100) & ~0xFu;
  uint32_t reg = 3;
  for (uint32_t a : args) c.r[reg++] = a;
  c.r[12] = r12;
  c.lr = kHandlerReturn;
  ppc::Dispatch(c, fn, 0);
  uint32_t r = uint32_t(c.r[3]);
  c = saved;
  c.resv_valid = false;
  return r;
}

// One frame up from p: false at the end of the stack (a function with no
// frame description, a frame outside the stack). `frame` is the frame's
// establisher frame (r1 on entry to its function).
bool Up(Point& p, uint32_t& frame) {
  const ppc::LiftedFrame* f = ppc::LookupFrame(p.pc);
  if (!f) return false;
  Ctx next = p.s;
  frame = ppc::VirtualUnwind(*f, next);
  uint32_t limit, base;
  GuestStackRange(limit, base);
  if (frame < uint32_t(p.s.r[1]) || frame > base || frame < limit) return false;
  p = {next, uint32_t(next.lr) - 4};
  return true;
}

// The handler of the function holding pc and its .pdata entry, or 0.
uint32_t HandlerAt(uint32_t pc, uint32_t& entry) {
  entry = FunctionEntry(pc);
  if (!entry || (Rd32(entry + 4) & kPdataHandler) != kPdataHandler) return 0;
  return Rd32(Rd32(entry) - 8);
}

// The walk reached the return address of a handler call: it continues at
// the start of the dispatch that called the handler (the frames between
// are the dispatcher's, which have no handlers of their own here). Returns
// that dispatch, null if there is none.
Active* Leave(Point& p, size_t& depth) {
  if (!depth) return nullptr;
  Active* a = t_active[--depth];
  p = a->start;
  return a;
}

// Continues in the frame that state s with Iar `ip` is in: a return from
// the kernel call if that is the caller's own frame at the return address,
// else a GuestResume to the function's activation.
uint64_t Continue(Ctx& c, Ctx s, uint32_t ip) {
  s.resv_valid = false;
  if (uint32_t(s.r[1]) == uint32_t(c.r[1]) && ip == uint32_t(c.lr)) {
    c = s;
    return c.r[3];
  }
  uint32_t frame = uint32_t(s.r[1]);
  if (const ppc::LiftedFrame* f = ppc::LookupFrame(ip)) {
    Ctx probe = s;
    frame = ppc::VirtualUnwind(*f, probe);
  }
  c = s;
  throw ppc::GuestResume{ip, frame};
}

// The first pass, for the exception `record` raised with `context` at
// `start`: true if a handler continued execution. `dc` is guest memory for
// the dispatcher context, the lowest of the dispatch's structures.
bool Dispatch(Ctx& c, uint32_t record, uint32_t context, uint32_t dc, const Point& start) {
  size_t depth = t_active.size();
  Active self{start, false, 0};
  Push push(&self);
  uint32_t nested = 0;
  Point p = start;
  for (;;) {
    if (p.pc == kHandlerReturn - 4) {
      // An exception in a handler: the frames from the outer exception's
      // start up to the frame whose handler raised it are searched again,
      // flagged as nested.
      Active* outer = Leave(p, depth);
      if (!outer) return false;
      if (outer->unwind) Fatal(&c, "an exception raised in a handler during an unwind: not modelled");
      nested = outer->frame;
      Wr32(record + kRecFlags, Rd32(record + kRecFlags) | kNestedCall);
    }
    Point here = p;
    uint32_t frame;
    if (!Up(p, frame)) return false;
    uint32_t entry, handler = HandlerAt(here.pc, entry);
    if (handler) {
      Wr32(dc + 0x0, here.pc);
      Wr32(dc + 0x4, entry);
      Wr32(dc + 0x8, frame);
      Wr32(dc + 0xC, context);
      self.frame = frame;
      uint32_t d = CallHandler(c, dc, handler, {record, frame, context, dc}, frame);
      if (frame == nested) {
        Wr32(record + kRecFlags, Rd32(record + kRecFlags) & ~kNestedCall);
        nested = 0;
      }
      if (d == kContinueExecution) {
        if (Rd32(record + kRecFlags) & kNoncontinuable)
          Fatal(&c, "a handler continued the noncontinuable exception %08X", Rd32(record));
        return true;
      }
      if (d != kContinueSearch)
        Fatal(&c, "exception %08X: handler %08X returned disposition %u", Rd32(record), handler, d);
    }
  }
}

// RtlUnwind's second pass, from the kernel call in c: calls the handlers
// of the frames up to target_frame (all of them with target_frame 0) as
// unwinding, then continues in the target frame at target_ip with r3 =
// value.
uint64_t Unwind(Ctx& c, uint32_t target_frame, uint32_t target_ip, uint32_t record,
                uint32_t value) {
  size_t depth = t_active.size();
  Active self{At(c), true, target_ip};
  Push push(&self);
  const uint32_t scratch = Scratch(c, kRecordSize + kContextSize + kDcSize);
  const uint32_t ctx = scratch + kRecordSize, dc = ctx + kContextSize;
  if (!record) {
    record = scratch;
    std::memset(Mem() + record, 0, kRecordSize);
    Wr32(record, kStatusUnwind);
    Wr32(record + kRecAddress, uint32_t(c.lr));
  }
  uint32_t flags = Rd32(record + kRecFlags) | kUnwinding;
  if (!target_frame) flags |= kExitUnwind;
  Point p = self.start;
  for (;;) {
    if (p.pc == kHandlerReturn - 4) {
      Active* outer = Leave(p, depth);
      if (!outer) Fatal(&c, "unwind to frame %x: it is not on the stack", target_frame);
      if (outer->unwind) Fatal(&c, "an unwind in a handler during an unwind: not modelled");
    }
    Point here = p;
    uint32_t frame;
    if (!Up(p, frame)) {
      if (!target_frame)
        Fatal(&c, "an exit unwind reached the end of the stack (status %08X)", Rd32(record));
      Fatal(&c, "unwind to frame %x: it is not on the stack", target_frame);
    }
    if (target_frame && frame > target_frame)
      Fatal(&c, "unwind to frame %x: passed it at frame %x", target_frame, frame);
    uint32_t entry, handler = HandlerAt(here.pc, entry);
    if (handler) {
      Wr32(record + kRecFlags, flags | (frame == target_frame ? kTargetUnwind : 0));
      WriteContext(ctx, here.s, here.pc + 4);
      Wr32(dc + 0x0, here.pc);
      Wr32(dc + 0x4, entry);
      Wr32(dc + 0x8, frame);
      Wr32(dc + 0xC, ctx);
      uint32_t d = CallHandler(c, scratch, handler, {record, frame, ctx, dc}, frame);
      if (d != kContinueSearch)
        Fatal(&c, "unwind: handler %08X returned disposition %u", handler, d);
      // The target frame's handler may change the context it continues in.
      if (frame == target_frame) ReadContext(ctx, here.s);
    }
    if (frame == target_frame) {
      Wr32(record + kRecFlags, flags);
      here.s.r[3] = value;
      return Continue(c, here.s, target_ip);
    }
  }
}

// ---- the exports ----------------------------------------------------------

// RtlCaptureContext(context): the caller's registers at the call, Iar its
// return address (as Windows NT's). r3 is unchanged: titles' C runtime
// passes it on as the context's address.
HLE(kKrnl, 281, RtlCaptureContext) {
  WriteContext(Arg(c, 0), c, uint32_t(c.lr));
  return c.r[3];
}

// RtlRaiseException(record): the exception's address is the caller's
// return address, its context the caller's registers. A handler that
// continues it continues there with the context it left; one that takes
// it unwinds (__C_specific_handler, a title's own) and does not return
// here. No handler: the title stops (what the console does then is a
// console check).
HLE(kKrnl, 310, RtlRaiseException) {
  uint32_t record = Arg(c, 0);
  Wr32(record + kRecAddress, uint32_t(c.lr));
  const uint32_t dc = Scratch(c, kDcSize + kContextSize), ctx = dc + kDcSize;
  WriteContext(ctx, c, uint32_t(c.lr));
  if (!Dispatch(c, record, ctx, dc, At(c))) {
    uint32_t n = Rd32(record + kRecParams);
    Fatal(&c, "unhandled exception %08X at %08X (flags %X, %u parameters: %08X %08X)", Rd32(record),
          uint32_t(c.lr), Rd32(record + kRecFlags), n, n ? Rd32(record + 0x14) : 0,
          n > 1 ? Rd32(record + 0x18) : 0);
  }
  Ctx s = c;
  uint32_t ip = ReadContext(ctx, s);
  return Continue(c, s, ip);
}

// RtlUnwind(target_frame, target_ip, record, value): see Unwind. No record:
// STATUS_UNWIND's, raised at the caller's return address.
HLE(kKrnl, 327, RtlUnwind) {
  return Unwind(c, Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3));
}

// __C_specific_handler(record, frame, context, dispatcher_context): the
// language handler of __try blocks, as Windows NT's for RISC processors.
// Its data (the word before the function, after the handler's address) is
// a count and per scope: its range, its filter (1: always) or __finally
// block, its __except block (0 for a __finally). Scopes are innermost first;
// the one holding ControlPc applies.
// - Dispatching: a filter is called with r3 the EXCEPTION_POINTERS (record,
//   context), r4 and r12 the frame; > 0 unwinds to the __except block with
//   r3 the exception code, < 0 continues execution, 0 searches on.
// - Unwinding: each __finally holding ControlPc is called with r3 1
//   (abnormal termination), r4 and r12 the frame, up to the __except the
//   unwind targets in its own frame.
// The registers the funclets get the frame in: r12 as titles' __finally
// blocks read it; r4 as Windows NT passes it (console check).
HLE(kKrnl, 421, __C_specific_handler) {
  uint32_t record = Arg(c, 0), frame = Arg(c, 1), context = Arg(c, 2), dc = Arg(c, 3);
  uint32_t pc = Rd32(dc), entry = Rd32(dc + 4);
  uint32_t scopes = Rd32(Rd32(entry) - 4), n = Rd32(scopes);
  uint32_t flags = Rd32(record + kRecFlags);
  if (!(flags & (kUnwinding | kExitUnwind))) {
    const uint32_t ptrs = Scratch(c, 8);
    Wr32(ptrs, record);
    Wr32(ptrs + 4, context);
    for (uint32_t k = 0; k < n; k++) {
      uint32_t s = scopes + 4 + 16 * k;
      uint32_t begin = Rd32(s), end = Rd32(s + 4), filter = Rd32(s + 8), target = Rd32(s + 12);
      if (pc < begin || pc >= end || !target) continue;
      int32_t v = filter == 1 ? 1 : int32_t(CallHandler(c, ptrs, filter, {ptrs, frame}, frame));
      if (v < 0) return kContinueExecution;
      if (v > 0) return Unwind(c, frame, target, record, Rd32(record));
    }
    return kContinueSearch;
  }
  uint32_t target_ip = 0;
  for (auto it = t_active.rbegin(); it != t_active.rend(); ++it)
    if ((*it)->unwind) {
      target_ip = (*it)->target_ip;
      break;
    }
  for (uint32_t k = 0; k < n; k++) {
    uint32_t s = scopes + 4 + 16 * k;
    uint32_t begin = Rd32(s), end = Rd32(s + 4), handler = Rd32(s + 8), target = Rd32(s + 12);
    if (pc < begin || pc >= end) continue;
    if ((flags & kTargetUnwind) && target == target_ip) break;
    if (!target) CallHandler(c, uint32_t(c.r[1]), handler, {1, frame}, frame);
  }
  return kContinueSearch;
}

// KeBugCheckEx(code, p1..p4): the system stops. Here the run ends with the
// code and parameters; what the console shows is a console check.
HLE(kKrnl, 83, KeBugCheckEx) {
  Fatal(&c, "bug check %08X (%08X, %08X, %08X, %08X)", Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3),
        Arg(c, 4));
}

// KeBugCheck(code): KeBugCheckEx with no parameters.
HLE(kKrnl, 82, KeBugCheck) { Fatal(&c, "bug check %08X (0, 0, 0, 0)", Arg(c, 0)); }

}  // namespace
}  // namespace recomp::xbox360
