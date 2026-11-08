#pragma once
// Walking the guest stack (ARCHITECTURE.md, "Unwinding"): the lifted
// modules' frame descriptions (LiftedFrame, from their prologues) and one
// frame's virtual unwind. What a platform's exception dispatch is built on;
// nothing here knows a console's exception structures.

#include <cstdint>

#include "ppc/lifted.h"

namespace recomp::ppc {

// The description of the function containing pc, or null (a function the
// image gives no prologue for: a leaf, or no lifted code).
const LiftedFrame* LookupFrame(uint32_t pc);

// From the state `s` inside the function of `f`, past its prologue, to its
// caller's at the return: r1 back to its value on entry, LR (the return
// address) and the registers the prologue saved back to theirs; the others
// are left as they are. Returns r1 on entry (the frame's establisher frame).
// Fatal if the prologue is unreadable.
uint32_t VirtualUnwind(const LiftedFrame& f, Ctx& s);

}  // namespace recomp::ppc
