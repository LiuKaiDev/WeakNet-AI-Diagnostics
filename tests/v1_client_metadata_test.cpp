#include "weaknet_client.h"

#include <iostream>
#include <string>

int main() {
    char version[256]{};
    char build[256]{};
    if (!weaknet_get_version(version, sizeof(version))) {
        std::cerr << "weaknet_get_version failed\n";
        return 1;
    }
    if (!weaknet_get_build_info(build, sizeof(build))) {
        std::cerr << "weaknet_get_build_info failed\n";
        return 1;
    }
    if (std::string(version) != "WeakNet Client Library v1.0.0") {
        std::cerr << "unexpected version: " << version << '\n';
        return 1;
    }
    if (std::string(build).find("C++20") == std::string::npos) {
        std::cerr << "build metadata does not report C++20: " << build << '\n';
        return 1;
    }
    return 0;
}
