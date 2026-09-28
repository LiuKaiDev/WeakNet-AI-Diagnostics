#include "ping_executor.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

}  // namespace

int main() {
    bool ok = true;
    std::mutex gate_mutex;
    std::condition_variable gate;
    bool release = false;
    std::atomic<int> started{0};
    std::atomic<int> completed{0};

    weaknet_dbus::PingExecutor executor(
        2, [&](std::stop_token token, const std::string&, const std::string&, int) {
            ++started;
            std::unique_lock lock(gate_mutex);
            gate.wait_for(lock, 2s, [&] { return release || token.stop_requested(); });
            return 17;
        });

    auto completion = [&](int result) {
        if (result == 17) ++completed;
    };
    ok &= expect(executor.submit("one", "test0", 100, completion), "first Ping rejected");
    for (int count = 0; count < 100 && started.load() == 0; ++count) {
        std::this_thread::sleep_for(5ms);
    }
    ok &= expect(started.load() == 1, "Ping worker did not start");
    ok &= expect(executor.submit("two", "test0", 100, completion), "second Ping rejected");
    ok &= expect(executor.submit("three", "test0", 100, completion), "third Ping rejected");
    ok &= expect(!executor.submit("four", "test0", 100, completion),
                 "bounded Ping queue accepted overflow");

    {
        std::lock_guard lock(gate_mutex);
        release = true;
    }
    gate.notify_all();
    for (int count = 0; count < 200 && completed.load() != 3; ++count) {
        std::this_thread::sleep_for(5ms);
    }
    ok &= expect(completed.load() == 3, "accepted Ping tasks did not complete");

    const auto before_stop = std::chrono::steady_clock::now();
    executor.stop();
    const auto stop_time = std::chrono::steady_clock::now() - before_stop;
    ok &= expect(stop_time < 500ms, "Ping executor stop exceeded 500 ms");
    ok &= expect(!executor.submit("after-stop", "test0", 100, completion),
                 "stopped Ping executor accepted work");
    return ok ? 0 : 1;
}
