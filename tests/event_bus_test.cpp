#include "event_bus.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <future>
#include <iostream>
#include <latch>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

using namespace weaknet_dbus::v2;

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

NetworkEvent gauge(std::uint64_t id, std::uint32_t ifindex = 1) {
    NetworkEventHeader header{kNetworkEventSchemaVersion, EventId{id}, {},
        EventKind::ProbeRttMetric, EventSource::V1RttMonitor,
        RealtimeTime(std::chrono::nanoseconds(id)), MonotonicTime(std::chrono::nanoseconds(id)),
        NetnsId{1, 2}, InterfaceId{ifindex, "test"}, std::nullopt,
        Validity::Valid, std::nullopt};
    return NetworkEvent(header, ProbeRttObservation{static_cast<double>(id)});
}

NetworkEvent transition(std::uint64_t id) {
    NetworkEventHeader header{kNetworkEventSchemaVersion, EventId{id}, {},
        EventKind::UplinkObservation, EventSource::V1UplinkMonitor,
        RealtimeTime{}, MonotonicTime{}, NetnsId{1, 2}, InterfaceId{1, "test"},
        std::nullopt, Validity::Partial, std::nullopt};
    return NetworkEvent(header, UplinkObservation{"test", 1, true});
}
}

int main() {
    bool ok = true;
    {
        EventBus bus(512);
        ok &= expect(bus.start(), "bus did not start");
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<std::uint64_t> dispatched;
        auto token = bus.subscribe({}, [&](const NetworkEvent& event) {
            std::lock_guard lock(mutex);
            dispatched.push_back(event.header().sequence.value);
            cv.notify_all();
        });
        std::vector<std::uint64_t> accepted;
        std::mutex accepted_mutex;
        std::vector<std::jthread> producers;
        for (std::uint64_t producer = 0; producer < 4; ++producer) {
            producers.emplace_back([&, producer] {
                for (std::uint64_t index = 0; index < 50; ++index) {
                    const auto result = bus.publish(gauge(1 + producer * 50 + index,
                                                          1 + producer));
                    if (result.accepted) {
                        std::lock_guard lock(accepted_mutex);
                        accepted.push_back(result.sequence.value);
                    }
                }
            });
        }
        producers.clear();
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return dispatched.size() == 200; });
        }
        bus.stop();
        std::sort(accepted.begin(), accepted.end());
        ok &= expect(dispatched == accepted, "multi-producer dispatch order differs from admission order");
        ok &= expect(bus.telemetry().accepted_publications == 200, "accepted count mismatch");
        bus.stop();
    }

    {
        EventBus bus(16);
        bus.start();
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<int> calls{};
        std::optional<EventBus::Subscription> added;
        EventBus::Subscription self;
        self = bus.subscribe({}, [&](const NetworkEvent& event) {
            calls.fetch_add(1);
            if (event.header().event_id.value == 1) {
                self.unsubscribe();
                added.emplace(bus.subscribe(EventFilter{EventKind::ProbeRttMetric,
                                                        std::nullopt, std::nullopt},
                    [&](const NetworkEvent&) { calls.fetch_add(1); cv.notify_all(); }));
                bus.publish(gauge(2));
            }
        });
        auto throwing = bus.subscribe({}, [](const NetworkEvent&) { throw 7; });
        bus.publish(gauge(1));
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return calls.load() >= 2; });
        }
        bus.stop();
        ok &= expect(!self.active(), "self-unsubscribe remained active");
        ok &= expect(bus.telemetry().callback_failures == 2,
                     "throwing callback was not contained/countable");
        EventBus::Subscription moved = std::move(*added);
        ok &= expect(moved.active() && !added->active(), "subscription move semantics failed");
    }

    {
        EventBus bus(2);
        bus.start();
        std::mutex mutex;
        std::condition_variable entered_cv;
        std::condition_variable release_cv;
        bool entered = false;
        bool release = false;
        auto token = bus.subscribe({}, [&](const NetworkEvent&) {
            std::unique_lock lock(mutex);
            if (!entered) {
                entered = true;
                entered_cv.notify_all();
                release_cv.wait(lock, [&] { return release; });
            }
        });
        bus.publish(gauge(1));
        {
            std::unique_lock lock(mutex);
            entered_cv.wait(lock, [&] { return entered; });
        }
        ok &= expect(bus.publish(gauge(2, 1)).accepted, "queue fill publish 1 failed");
        ok &= expect(bus.publish(gauge(3, 2)).accepted, "queue fill publish 2 failed");
        const auto coalesced = bus.publish(gauge(4, 1));
        ok &= expect(coalesced.accepted && coalesced.coalesced, "identical-key gauge did not coalesce");
        ok &= expect(!bus.publish(gauge(5, 3)).accepted, "different-key gauge coalesced incorrectly");
        ok &= expect(!bus.publish(transition(6)).accepted, "nonreplaceable event was not rejected at capacity");
        auto unsubscribe_future = std::async(std::launch::async, [&] { token.unsubscribe(); });
        ok &= expect(unsubscribe_future.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
                     "concurrent unsubscribe did not synchronize with callback");
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        release_cv.notify_all();
        unsubscribe_future.get();
        bus.stop();
        const auto telemetry = bus.telemetry();
        ok &= expect(telemetry.high_water_mark == 2, "queue high-water mismatch");
        ok &= expect(telemetry.total_coalesces == 1, "coalesce count mismatch");
        ok &= expect(telemetry.total_drops == 2, "drop count mismatch");
        ok &= expect(telemetry.accepted_publications == 4, "saturation accepted count mismatch");
        ok &= expect(telemetry.dispatched_publications == 3, "drain dispatch count mismatch");
    }

    {
        EventBus bus(8);
        bus.start();
        std::atomic<int> calls{};
        {
            auto token = bus.subscribe({}, [&](const NetworkEvent&) { ++calls; });
        }
        bus.publish(gauge(1));
        bus.stop();
        ok &= expect(calls.load() == 0, "RAII destruction allowed a future callback");
        ok &= expect(!bus.publish(gauge(2)).accepted, "publish after stop was accepted");
    }

    {
        EventBus bus(32);
        bus.start();
        std::latch ready(4);
        std::latch release(1);
        std::vector<std::jthread> producers;
        for (std::uint64_t producer = 0; producer < 4; ++producer) {
            producers.emplace_back([&, producer] {
                ready.count_down();
                release.wait();
                for (std::uint64_t index = 0; index < 2000; ++index) {
                    bus.publish(gauge(10000 + producer * 2000 + index,
                                      static_cast<std::uint32_t>(producer + 1)));
                }
            });
        }
        ready.wait();
        release.count_down();
        std::jthread first_stop([&] { bus.stop(); });
        std::jthread second_stop([&] { bus.stop(); });
        producers.clear();
        first_stop.join();
        second_stop.join();
        ok &= expect(!bus.running(), "concurrent stop with active producers failed");
    }
    return ok ? 0 : 1;
}
