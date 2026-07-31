#pragma once
#include <string>
#include "evgrpc/charging.pb.h"  // for ChargerType enum

namespace evgrpc {

// Bridge between the proto `ChargerType` enum (carried as int32 over
// the wire) and the SQL `charger_type_enum` (stored as the label
// string 'FAST' / 'SLOW').
//
// `CHARGER_TYPE_UNSPECIFIED` (proto default 0) maps to SQL NULL on
// insert — the caller should pick an explicit type when persisting.
//
// Used by both ChargingService (Task 14) and DisplayService (Task 17+).
// Defined in `util/` so neither service has to leak its anonymous
// namespace to the other.

const char* ChargerTypeLabel(ChargerType t);
ChargerType ChargerTypeFromLabel(const std::string& s);

}  // namespace evgrpc