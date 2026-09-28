#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace weaknet_dbus {

inline bool waitForStop(std::stop_token token, std::chrono::milliseconds duration) {
    std::mutex mutex;
    std::unique_lock lock(mutex);
    std::condition_variable_any condition;
    condition.wait_for(lock, token, duration, [] { return false; });
    return token.stop_requested();
}

}  // namespace weaknet_dbus
