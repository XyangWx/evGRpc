#include "util/args.h"
#include <stdexcept>
#include <string>

namespace evgrpc {

ArgvResult ParseArgs(int argc, char** argv) {
    ArgvResult r;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            r.help_requested = true;
        } else if (a == "--config" || a == "-c") {
            if (i + 1 >= argc) {
                throw std::runtime_error(
                    "missing value for " + a);
            }
            r.config_path = argv[++i];
        } else {
            throw std::runtime_error("unknown flag: " + a);
        }
    }
    return r;
}

}  // namespace evgrpc
