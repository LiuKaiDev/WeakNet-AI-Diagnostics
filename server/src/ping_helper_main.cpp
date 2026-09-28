#include "net_ping.h"

#include <charconv>
#include <cstdio>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 4) return 64;
    int timeout_ms = 0;
    const std::string_view timeout(argv[3]);
    const auto conversion = std::from_chars(
        timeout.data(), timeout.data() + timeout.size(), timeout_ms);
    if (conversion.ec != std::errc{} || conversion.ptr != timeout.data() + timeout.size() ||
        timeout_ms <= 0) {
        return 64;
    }
    const int result = NetPing::getInstance()->ping(argv[1], argv[2], timeout_ms);
    std::printf("%d\n", result);
    return 0;
}
