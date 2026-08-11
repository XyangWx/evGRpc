#include "services/charger/charger_type.h"

namespace evgrpc {

const char* ChargerTypeLabel(ChargerType t) {
  switch (t) {
    case ChargerType::CHARGER_TYPE_FAST: return "fast";
    case ChargerType::CHARGER_TYPE_SLOW: return "slow";
    default: return nullptr;  // UNSPECIFIED -> NULL via SQL
  }
}

ChargerType ChargerTypeFromLabel(const std::string& s) {
  if (s == "fast") return ChargerType::CHARGER_TYPE_FAST;
  if (s == "slow") return ChargerType::CHARGER_TYPE_SLOW;
  return ChargerType::CHARGER_TYPE_UNSPECIFIED;
}

}  // namespace evgrpc