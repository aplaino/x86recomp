// The 16-byte compare-exchange of CMPXCHG16B (lifted.h). The compiler's
// own atomic on a 16-byte integer, which every supported host does in one
// instruction (CMPXCHG16B with -mcx16 on x86-64; CASP or LDAXP/STLXP on
// arm64), so it is lock-free and atomic against the guest's other threads.

#include <cstdint>

#include "runtime/lifted.h"

namespace recomp::lift {

bool Cas128(uint8_t* p, uint64_t* lo, uint64_t* hi, uint64_t new_lo, uint64_t new_hi) {
  using U128 = unsigned __int128;
  U128 e = U128(*hi) << 64 | *lo;
  const U128 n = U128(new_hi) << 64 | new_lo;
  const bool ok = __atomic_compare_exchange_n(reinterpret_cast<U128*>(p), &e, n, false,
                                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  *lo = uint64_t(e);
  *hi = uint64_t(e >> 64);
  return ok;
}

}  // namespace recomp::lift
