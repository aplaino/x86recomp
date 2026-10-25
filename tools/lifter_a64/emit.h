#pragma once
// C++ emission: one `void f_<VA>(Ctx&)` per guest function, general
// registers, SP and NZCV cached in locals and spilled around anything that
// can observe them (platforms/horizon/ARCHITECTURE.md, "Lifter").

#include <string>

#include "program.h"

namespace lifter::a64 {

std::string FunctionName(uint64_t va);
std::string EmitFunction(const Program& p, const Function& f);
// One instruction as a whole function (`name`) that loads and spills every
// register: the instruction form tests run these.
std::string EmitForm(const Program& p, const a64::Insn& i, const std::string& name);

}  // namespace lifter::a64
