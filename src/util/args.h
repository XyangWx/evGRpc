#pragma once
#include <string>

namespace evgrpc {

struct ArgvResult {
    std::string config_path = "./config.json";  // default "./config.json"
    bool help_requested{false};
};

// Recognized flags:
//   --config <path> | -c <path>  (sets config_path)
//   --help | -h                  (sets help_requested = true)
//
// Throws std::runtime_error on unknown flag or missing value.
ArgvResult ParseArgs(int argc, char** argv);

}  // namespace evgrpc
