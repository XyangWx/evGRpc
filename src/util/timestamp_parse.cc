#include "util/timestamp_parse.h"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace evgrpc {

namespace {
// Length of the "YYYY-MM-DD HH:MM:SS" prefix that every accepted form
// shares. Anything after it is either a UTC offset ("+08", "+05:30",
// "-08", "Z") or nothing.
constexpr size_t kDateTimeLen = 19;
}  // namespace

bool ParseTimestamp(const std::string& s, google::protobuf::Timestamp* out) {
  if (s.empty()) return false;

  std::string body = s;

  // 1. Drop fractional seconds but KEEP any trailing offset
  //    ("...22:13:20.123456+00" -> "...22:13:20+00").
  const auto dot = body.find('.');
  if (dot != std::string::npos) {
    const auto tzpos = body.find_first_of("+-Z", dot);
    body = (tzpos != std::string::npos)
               ? body.substr(0, dot) + body.substr(tzpos)
               : body.substr(0, dot);
  }

  // 2. Split a trailing UTC offset. TIMESTAMPTZ `::text` appends the
  //    session offset ("+08", "+05:30", "-08", or "Z"); a bare TIMESTAMP
  //    has none. The offset is the number of seconds *east* of UTC.
  long offset_sec = 0;
  if (body.size() > kDateTimeLen) {
    const std::string tz = body.substr(kDateTimeLen);
    body.resize(kDateTimeLen);
    if (tz == "Z") {
      offset_sec = 0;
    } else if ((tz.size() == 3 || tz.size() == 6) &&
               (tz[0] == '+' || tz[0] == '-') &&
               tz[1] >= '0' && tz[1] <= '9' &&
               tz[2] >= '0' && tz[2] <= '9') {
      const int sign = (tz[0] == '-') ? -1 : 1;
      const int hh = (tz[1] - '0') * 10 + (tz[2] - '0');
      if (hh > 14) return false;  // real-world UTC offsets are within ±14:00
      int mm = 0;
      if (tz.size() == 6) {
        if (tz[3] != ':' || tz[4] < '0' || tz[4] > '9' ||
            tz[5] < '0' || tz[5] > '9') {
          return false;
        }
        mm = (tz[4] - '0') * 10 + (tz[5] - '0');
        if (mm > 59) return false;
      }
      offset_sec = sign * (hh * 3600 + mm * 60);
    } else {
      return false;  // unknown suffix
    }
  }

  // 3. Parse the wall-clock date-time.
  std::tm tm{};
  std::istringstream iss(body);
  iss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
  if (iss.fail()) return false;
  // Reject trailing garbage (e.g. an offset we failed to split).
  if (iss.peek() != std::istringstream::traits_type::eof()) return false;

  // 4. timegm interprets `tm` as UTC (POSIX/glibc/BSD; mktime would
  //    apply the process-local TZ). Linux target only — use _mkgmtime
  //    if MSVC support is ever needed. Subtract the east-of-UTC offset
  //    to recover the actual instant: "22:13:20+08" is 14:13:20 UTC.
  out->set_seconds(timegm(&tm) - offset_sec);
  out->set_nanos(0);
  return true;
}

}  // namespace evgrpc
