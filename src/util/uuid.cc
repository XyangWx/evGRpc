#include "util/uuid.h"

#include <uuid/uuid.h>

namespace evgrpc {

std::string NewUuid() {
  uuid_t u;
  uuid_generate(u);
  char buf[37];  // 36 chars + NUL
  uuid_unparse_lower(u, buf);
  return std::string(buf);
}

}  // namespace evgrpc