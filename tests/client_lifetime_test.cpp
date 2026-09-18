#include "weaknet_client.h"

#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    std::atomic<bool> failed{false};
    if (!weaknet_init()) {
        std::cerr << "initial client connection failed\n";
        return 1;
    }

    std::vector<std::thread> readers;
    for (int index = 0; index < 4; ++index) {
        readers.emplace_back([&] {
            char version[128]{};
            for (int iteration = 0; iteration < 500; ++iteration) {
                if (!weaknet_get_version(version, sizeof(version))) failed.store(true);
                (void)weaknet_is_connected();
            }
        });
    }
    std::thread lifecycle([&] {
        for (int iteration = 0; iteration < 100; ++iteration) {
            weaknet_cleanup();
            if (!weaknet_init()) failed.store(true);
        }
    });

    for (auto& reader : readers) reader.join();
    lifecycle.join();
    weaknet_cleanup();
    weaknet_cleanup();
    return failed.load() ? 1 : 0;
}
