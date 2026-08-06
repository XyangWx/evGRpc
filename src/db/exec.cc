#include "db/exec.h"
#include <sstream>

namespace evgrpc::db {

namespace detail {

// Truncate string-like values to 64 chars + "...".
template <typename String>
std::string TruncateStr(String s) {
    if (s.size() > 64) {
        // s.substr() returns String (a view for string_view); explicitly
        // materialize a std::string before concatenating the "..." marker.
        return std::string(s.substr(0, 64)) + "...";
    }
    return std::string(s);
}

std::string FormatParam(const std::string& s) { return "\"" + TruncateStr(s) + "\""; }
std::string FormatParam(std::string_view s) { return "\"" + TruncateStr(s) + "\""; }
std::string FormatParam(const char* s) { return FormatParam(std::string(s ? s : "")); }
std::string FormatParam(bool b) { return b ? "true" : "false"; }
std::string FormatParam(int n) { return std::to_string(n); }
std::string FormatParam(long n) { return std::to_string(n); }
std::string FormatParam(long long n) { return std::to_string(n); }
std::string FormatParam(unsigned n) { return std::to_string(n); }
std::string FormatParam(unsigned long n) { return std::to_string(n); }
std::string FormatParam(unsigned long long n) { return std::to_string(n); }
std::string FormatParam(double d) {
    std::ostringstream os; os << d; return os.str();
}
std::string FormatParam(float f) { return FormatParam(static_cast<double>(f)); }

}  // namespace detail

}  // namespace evgrpc::db
