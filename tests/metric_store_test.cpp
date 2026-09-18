#include "metric_store.hpp"

#include <atomic>
#include <iostream>
#include <thread>

using namespace weaknet_dbus::v2;

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

MetricKey key(NetnsId ns = {1, 1}, std::uint32_t ifindex = 1,
              std::optional<SocketId> socket = std::nullopt) {
    return MetricKey{ns, MetricName::Rtt, MetricUnit::Milliseconds,
                     EventSource::V1RttMonitor, InterfaceId{ifindex, "test"},
                     socket, 1000};
}

MetricSample sample(std::uint64_t id, const MetricKey& metric_key,
                    MonotonicTime time, Validity validity = Validity::Valid,
                    std::optional<MetricValue> value = MetricValue{double{1.0}}) {
    return MetricSample{SampleId{id}, metric_key, value, validity,
                        RealtimeTime(time.time_since_epoch()), time, std::nullopt};
}
}

int main() {
    bool ok = true;
    ManualClock clock(RealtimeTime{}, MonotonicTime{});
    MetricStoreConfig config;
    config.max_series = 2;
    config.max_samples_per_series = 3;
    config.max_total_samples = 5;
    config.max_retained_age = std::chrono::seconds(10);
    config.max_query_results = 2;
    config.stale_after = std::chrono::seconds(3);
    MetricStore store(clock, config);

    const auto first_key = key();
    ok &= expect(!store.latest(first_key), "missing metric became a value");
    ok &= expect(store.insert(sample(1, first_key, MonotonicTime{},
                                     Validity::Unavailable, std::nullopt)),
                 "unavailable sample was rejected");
    auto unavailable = store.latest(first_key);
    ok &= expect(unavailable && unavailable->validity == Validity::Unavailable &&
                 !unavailable->value, "unavailable sample became numeric zero");

    clock.advance(std::chrono::seconds(1));
    ok &= expect(store.insert(sample(2, first_key, clock.monotonicNow(),
                                     Validity::Partial, MetricValue{2.0})),
                 "partial sample rejected");
    clock.advance(std::chrono::seconds(1));
    ok &= expect(store.insert(sample(3, first_key, clock.monotonicNow(),
                                     Validity::Reset, std::nullopt)),
                 "reset sample rejected");
    clock.advance(std::chrono::seconds(1));
    store.insert(sample(4, first_key, clock.monotonicNow(),
                        Validity::Valid, MetricValue{4.0}));
    ok &= expect(store.snapshot().front().samples.size() == 3,
                 "per-series retention did not evict oldest sample");
    auto limited = store.window(first_key, MonotonicTime{}, clock.monotonicNow(), 99);
    ok &= expect(limited.size() == 2, "query maximum was not enforced");

    auto immutable = store.snapshot();
    store.insert(sample(5, first_key, clock.monotonicNow(),
                        Validity::Valid, MetricValue{5.0}));
    ok &= expect(immutable.front().samples.back().sample_id.value == 4,
                 "snapshot aliased mutable store state");

    clock.advance(std::chrono::seconds(4));
    auto stale = store.latest(first_key);
    ok &= expect(stale && stale->validity == Validity::Stale,
                 "stale-after policy was not applied");

    const auto second_key = key(NetnsId{2, 1}, 1, SocketId{8, 1});
    store.insert(sample(6, second_key, clock.monotonicNow()));
    ok &= expect(store.snapshot(NetnsId{1, 1}).size() == 1 &&
                 store.snapshot(NetnsId{2, 1}).size() == 1,
                 "network namespace filtering failed");
    const auto third_key = key(NetnsId{1, 1}, 2, SocketId{8, 2});
    store.insert(sample(7, third_key, clock.monotonicNow()));
    ok &= expect(!store.latest(first_key), "deterministic oldest-series eviction failed");
    ok &= expect(store.latest(second_key) && store.latest(third_key),
                 "cardinality eviction removed the wrong series");

    MetricSample invalid = sample(8, third_key, clock.monotonicNow(),
                                  Validity::Unavailable, MetricValue{0.0});
    ok &= expect(!store.insert(invalid), "unavailable numeric sentinel was accepted");

    std::atomic<bool> writer_done{false};
    std::jthread writer([&] {
        for (std::uint64_t id = 10; id < 210; ++id) {
            clock.advance(std::chrono::nanoseconds(1));
            store.insert(sample(id, third_key, clock.monotonicNow(), Validity::Valid,
                                MetricValue{static_cast<double>(id)}));
        }
        writer_done.store(true);
    });
    while (!writer_done.load()) {
        const auto view = store.snapshot();
        for (const auto& series : view) {
            ok &= expect(series.samples.size() <= config.max_samples_per_series,
                         "concurrent snapshot exceeded retention bound");
        }
    }
    writer.join();
    const auto telemetry = store.telemetry();
    ok &= expect(telemetry.active_series <= config.max_series &&
                 telemetry.sample_count <= config.max_total_samples,
                 "store exceeded configured global bounds");
    ok &= expect(telemetry.series_evictions == 1, "series eviction telemetry mismatch");
    ok &= expect(telemetry.rejected_samples == 1, "rejected sample telemetry mismatch");
    ok &= expect(telemetry.stale_query_results > 0, "stale query telemetry missing");

    clock.advance(std::chrono::seconds(20));
    ok &= expect(!store.latest(third_key), "maximum retained age was not enforced");
    return ok ? 0 : 1;
}
