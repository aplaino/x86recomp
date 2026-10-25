#pragma once
// C++ emission: one `void f_<VA>(Ctx&)` per guest function, the general
// registers, CR, XER, LR and CTR it uses cached in locals and spilled
// around anything that can observe them (platforms/xbox360/ARCHITECTURE.md,
// "Lifter").

#include <string>

#include "program.h"

namespace lifter::ppc {

std::string FunctionName(uint64_t va);
std::string EmitFunction(const Program& p, const Function& f);
// One instruction as a whole function (`name`) that loads and spills every
// register: the instruction form tests run these.
std::string EmitForm(const Program& p, const ppc::Insn& i, const std::string& name);

}  // namespace lifter::ppc
