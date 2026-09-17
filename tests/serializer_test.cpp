#include "serializer.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    using namespace weaknet_dbus;

    bool ok = true;
    std::vector<std::uint8_t> buffer;
    serializeString("WeakNet", buffer);
    serializeInt32(-2048, buffer);

    std::size_t offset = 0;
    std::string text;
    std::int32_t number = 0;
    ok &= expect(deserializeString(buffer, offset, text), "deserialize string");
    ok &= expect(text == "WeakNet", "string round trip");
    ok &= expect(deserializeInt32(buffer, offset, number), "deserialize int32");
    ok &= expect(number == -2048, "int32 round trip");
    ok &= expect(offset == buffer.size(), "consume complete buffer");

    std::vector<std::uint8_t> malformed{4, 0, 0, 0, 'a'};
    offset = 0;
    ok &= expect(!deserializeString(malformed, offset, text), "reject truncated string");

    const auto path = std::filesystem::temp_directory_path() /
        ("weaknet-serializer-" + std::to_string(::getpid()) + ".bin");
    const ChangedPayload original{"interface changed", 17};
    std::string error;
    ok &= expect(serializeChangedPayloadToFile(original, path.string(), &error),
                 "write payload file");
    ChangedPayload restored;
    ok &= expect(deserializeChangedPayloadFromFile(path.string(), &restored, &error),
                 "read payload file");
    ok &= expect(restored.message == original.message, "payload message round trip");
    ok &= expect(restored.counter == original.counter, "payload counter round trip");
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    ok &= expect(!remove_error, "remove temporary payload file");

    return ok ? 0 : 1;
}
