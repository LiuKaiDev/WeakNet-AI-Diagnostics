#pragma once

#include <atomic>
#include <chrono>

namespace weaknet_dbus::v2 {

using RealtimeTime = std::chrono::system_clock::time_point;
using MonotonicTime = std::chrono::steady_clock::time_point;

class Clock {
public:
    virtual ~Clock() = default;
    virtual RealtimeTime realtimeNow() const noexcept = 0;
    virtual MonotonicTime monotonicNow() const noexcept = 0;
};

class SystemClock final : public Clock {
public:
    RealtimeTime realtimeNow() const noexcept override;
    MonotonicTime monotonicNow() const noexcept override;
};

// Thread-safe manual clock used by deterministic unit and stress tests.
class ManualClock final : public Clock {
public:
    ManualClock(RealtimeTime realtime = RealtimeTime{},
                MonotonicTime monotonic = MonotonicTime{}) noexcept;

    RealtimeTime realtimeNow() const noexcept override;
    MonotonicTime monotonicNow() const noexcept override;
    void advance(std::chrono::nanoseconds duration) noexcept;
    void set(RealtimeTime realtime, MonotonicTime monotonic) noexcept;

private:
    std::atomic<std::int64_t> realtime_ns_;
    std::atomic<std::int64_t> monotonic_ns_;
};

}  // namespace weaknet_dbus::v2
