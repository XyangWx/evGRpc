#include <iostream>
#include "config/config.h"

int main() {
    try {
        auto c = evgrpc::Config::Load();
        std::cout << "evGRpc server starting on port " << c.grpc_port << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "config error: " << e.what() << std::endl;
        return 1;
    }
}
