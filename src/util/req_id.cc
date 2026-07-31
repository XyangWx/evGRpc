#include "util/req_id.h"

#include <iomanip>
#include <random>
#include <sstream>

namespace evgrpc {

std::string NewReqId() {
  // thread_local so per-thread rng doesn't contend. Per-call entropy
  // is 128 bits — more than enough for log correlation.
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  uint64_t a = rng();
  uint64_t b = rng();
  std::ostringstream os;
  os << std::hex << std::setw(16) << std::setfill('0') << a
     << std::setw(16) << std::setfill('0') << b;
  return os.str();
}

}  // namespace evgrpc