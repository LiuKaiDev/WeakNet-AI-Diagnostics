#include "clock.hpp"

namespace weaknet_dbus::v2 {
namespace {

template <typename Duration>
std::int64_t toNanoseconds(Duration value) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value).count();
}

}  // namespace

RealtimeTime SystemClock::realtimeNow() const noexcept {
    return std::chrono::system_clock::now();
}

MonotonicTime SystemClock::monotonicNow() const noexcept {
    return std::chrono::steady_clock::now();
}

ManualClock::ManualClock(RealtimeTime realtime, MonotonicTime monotonic) noexcept
    : realtime_ns_(toNanoseconds(realtime.time_since_epoch())),
      monotonic_ns_(toNanoseconds(monotonic.time_since_epoch())) {}

RealtimeTime ManualClock::realtimeNow() const noexcept {
    return RealtimeTime(std::chrono::nanoseconds(realtime_ns_.load()));
}

MonotonicTime ManualClock::monotonicNow() const noexcept {
    return MonotonicTime(std::chrono::nanoseconds(monotonic_ns_.load()));
}

void ManualClock::advance(std::chrono::nanoseconds duration) noexcept {
    realtime_ns_.fetch_add(duration.count());
    monotonic_ns_.fetch_add(duration.count());
}

void ManualClock::set(RealtimeTime realtime, MonotonicTime monotonic) noexcept {
    realtime_ns_.store(toNanoseconds(realtime.time_since_epoch()));
    monotonic_ns_.store(toNanoseconds(monotonic.time_since_epoch()));
}

}  // namespace weaknet_dbus::v2
