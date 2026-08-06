#include "config/config_loader.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace evgrpc {

namespace {

using json = nlohmann::json;

const std::set<std::string> kValidLogLevels = {
    "trace", "debug", "info", "warn", "error", "critical"};

const std::set<std::string> kAllowedDatabaseKeys = {"url"};
const std::set<std::string> kAllowedOAuthKeys = {
    "issuer_url", "audience", "jwks_cache_ttl_seconds"};
const std::set<std::string> kAllowedGrpcKeys = {"port"};
const std::set<std::string> kAllowedLogKeys = {
    "level", "file", "max_size_mb", "max_files"};

class ConfigErrorCollector {
public:
    void Add(const std::string& msg) { errors_.push_back(msg); }
    [[noreturn]] void Throw(const std::string& path) {
        std::ostringstream os;
        os << path;
        for (const auto& e : errors_) os << ": " << e << "\n";
        // Trim trailing newline.
        auto s = os.str();
        while (!s.empty() && s.back() == '\n') s.pop_back();
        throw std::runtime_error(s);
    }
    bool Empty() const { return errors_.empty(); }
private:
    std::vector<std::string> errors_;
};

std::string ReadFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error(path + ": cannot open file");
    }
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}

void CheckUnknownKeys(const json& obj, const std::set<std::string>& allowed,
                      const std::string& section,
                      ConfigErrorCollector& errs) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!allowed.count(it.key())) {
            std::ostringstream os;
            os << section << ": unknown key \"" << it.key()
               << "\" (allowed: ";
            bool first = true;
            for (const auto& k : allowed) {
                if (!first) os << ", ";
                os << k;
                first = false;
            }
            os << ")";
            errs.Add(os.str());
        }
    }
}

bool IsHttpUrl(const std::string& s) {
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

bool IsWritableDir(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return false;
    return access(path.c_str(), W_OK) == 0;
}

}  // namespace

SchemaConfig LoadSchema(const std::string& path) {
    std::string raw;
    try {
        raw = ReadFile(path);
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }

    json j;
    try {
        j = json::parse(raw);
    } catch (const json::parse_error& e) {
        // nlohmann::json::parse_error::what() contains the byte position;
        // we re-raise with path prefix and a cleaner message.
        throw std::runtime_error(
            path + ": parse error: " + e.what());
    }

    if (!j.is_object()) {
        throw std::runtime_error(
            path + ": top-level must be a JSON object");
    }

    ConfigErrorCollector errs;
    SchemaConfig cfg;

    // --- database ---
    if (!j.contains("database")) {
        errs.Add("database.url: missing required field");
    } else {
        const auto& db = j["database"];
        if (!db.is_object()) {
            errs.Add("database: must be an object");
        } else {
            CheckUnknownKeys(db, kAllowedDatabaseKeys, "database", errs);
            if (!db.contains("url")) {
                errs.Add("database.url: missing required field");
            } else if (!db["url"].is_string() || db["url"].get<std::string>().empty()) {
                errs.Add("database.url: must be a non-empty string");
            } else {
                auto s = db["url"].get<std::string>();
                if (s.rfind("postgresql://", 0) != 0) {
                    errs.Add("database.url: must start with \"postgresql://\" (got \"" + s + "\")");
                } else {
                    cfg.database.url = s;
                }
            }
        }
    }

    // --- oauth ---
    if (!j.contains("oauth")) {
        errs.Add("oauth: missing required field");
    } else {
        const auto& oa = j["oauth"];
        if (!oa.is_object()) {
            errs.Add("oauth: must be an object");
        } else {
            CheckUnknownKeys(oa, kAllowedOAuthKeys, "oauth", errs);
            if (!oa.contains("issuer_url")) {
                errs.Add("oauth.issuer_url: missing required field");
            } else if (!oa["issuer_url"].is_string() ||
                       oa["issuer_url"].get<std::string>().empty()) {
                errs.Add("oauth.issuer_url: must be a non-empty string");
            } else {
                auto s = oa["issuer_url"].get<std::string>();
                if (!IsHttpUrl(s)) {
                    errs.Add("oauth.issuer_url: must be a valid http(s) URL (got \"" + s + "\")");
                } else {
                    cfg.oauth.issuer_url = s;
                }
            }
            if (!oa.contains("audience")) {
                errs.Add("oauth.audience: missing required field");
            } else if (!oa["audience"].is_string() ||
                       oa["audience"].get<std::string>().empty()) {
                errs.Add("oauth.audience: must be a non-empty string");
            } else {
                cfg.oauth.audience = oa["audience"].get<std::string>();
            }
            if (oa.contains("jwks_cache_ttl_seconds")) {
                if (!oa["jwks_cache_ttl_seconds"].is_number_integer()) {
                    errs.Add("oauth.jwks_cache_ttl_seconds: must be an integer");
                } else {
                    int n = oa["jwks_cache_ttl_seconds"].get<int>();
                    if (n <= 0) {
                        errs.Add("oauth.jwks_cache_ttl_seconds: must be > 0 (got " + std::to_string(n) + ")");
                    } else if (n > 86400) {
                        errs.Add("oauth.jwks_cache_ttl_seconds: must be <= 86400 (got " + std::to_string(n) + ")");
                    } else {
                        cfg.oauth.jwks_cache_ttl_seconds = n;
                    }
                }
            }
        }
    }

    // --- grpc ---
    if (!j.contains("grpc")) {
        errs.Add("grpc: missing required field");
    } else {
        const auto& g = j["grpc"];
        if (!g.is_object()) {
            errs.Add("grpc: must be an object");
        } else {
            CheckUnknownKeys(g, kAllowedGrpcKeys, "grpc", errs);
            if (g.contains("port")) {
                if (!g["port"].is_number_integer()) {
                    errs.Add("grpc.port: must be an integer");
                } else {
                    int n = g["port"].get<int>();
                    if (n < 1 || n > 65535) {
                        errs.Add("grpc.port: must be in [1, 65535] (got " + std::to_string(n) + ")");
                    } else {
                        cfg.grpc.port = n;
                    }
                }
            }
        }
    }

    // --- log ---
    if (!j.contains("log")) {
        errs.Add("log: missing required field");
    } else {
        const auto& l = j["log"];
        if (!l.is_object()) {
            errs.Add("log: must be an object");
        } else {
            CheckUnknownKeys(l, kAllowedLogKeys, "log", errs);
            if (l.contains("level")) {
                if (!l["level"].is_string()) {
                    errs.Add("log.level: must be a string");
                } else {
                    auto s = l["level"].get<std::string>();
                    if (!kValidLogLevels.count(s)) {
                        errs.Add("log.level: must be one of trace/debug/info/warn/error/critical (got \"" + s + "\")");
                    } else {
                        cfg.log.level = s;
                    }
                }
            }
            if (l.contains("file")) {
                if (!l["file"].is_string()) {
                    errs.Add("log.file: must be a string");
                } else {
                    auto s = l["file"].get<std::string>();
                    if (!s.empty()) {
                        std::filesystem::path p(s);
                        auto parent = p.parent_path();
                        if (parent.empty()) parent = ".";
                        if (!IsWritableDir(parent.string())) {
                            errs.Add("log.file: parent directory does not exist or is not writable: " + parent.string());
                        } else {
                            cfg.log.file = s;
                        }
                    }
                }
            }
            if (l.contains("max_size_mb")) {
                if (!l["max_size_mb"].is_number_integer()) {
                    errs.Add("log.max_size_mb: must be an integer");
                } else {
                    int n = l["max_size_mb"].get<int>();
                    if (n <= 0) {
                        errs.Add("log.max_size_mb: must be > 0 (got " + std::to_string(n) + ")");
                    } else if (n > 1024) {
                        errs.Add("log.max_size_mb: must be <= 1024 (got " + std::to_string(n) + ")");
                    } else {
                        cfg.log.max_size_mb = n;
                    }
                }
            }
            if (l.contains("max_files")) {
                if (!l["max_files"].is_number_integer()) {
                    errs.Add("log.max_files: must be an integer");
                } else {
                    int n = l["max_files"].get<int>();
                    if (n <= 0) {
                        errs.Add("log.files: must be > 0 (got " + std::to_string(n) + ")");
                    } else if (n > 100) {
                        errs.Add("log.max_files: must be <= 100 (got " + std::to_string(n) + ")");
                    } else {
                        cfg.log.max_files = n;
                    }
                }
            }
        }
    }

    if (!errs.Empty()) errs.Throw(path);
    return cfg;
}

}  // namespace evgrpc
