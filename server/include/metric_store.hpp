#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

#include "clock.hpp"
#include "network_event.hpp"

namespace weaknet_dbus::v2 {

struct SampleId {
    std::uint64_t value{};
    auto operator<=>(const SampleId&) const = default;
};

enum class MetricName : std::uint8_t {
    Rtt,
    TcpLossRate,
    TrafficBytesPerSecond,
    TrafficPacketsPerSecond,
    TrafficActiveFlows,
    WifiRssi,
};

enum class MetricUnit : std::uint8_t {
    Milliseconds,
    Percent,
    BytesPerSecond,
    PacketsPerSecond,
    Count,
    DecibelMilliwatts,
};

using MetricValue = std::variant<std::int64_t, std::uint64_t, double>;

struct MetricKey {
    NetnsId netns;
    MetricName metric{MetricName::Rtt};
    MetricUnit unit{MetricUnit::Milliseconds};
    EventSource source{EventSource::Runtime};
    std::optional<InterfaceId> interface;
    std::optional<SocketId> socket;
    std::uint64_t observation_interval_ms{};

    auto operator<=>(const MetricKey&) const = default;
};

struct MetricSample {
    SampleId sample_id;
    MetricKey key;
    std::optional<MetricValue> value;
    Validity validity{Validity::Unavailable};
    RealtimeTime observed_at;
    MonotonicTime monotonic_at;
    std::optional<Status> status;
    auto operator<=>(const MetricSample&) const = default;
};

struct MetricStoreConfig {
    std::size_t max_series{1024};
    std::size_t max_samples_per_series{256};
    std::size_t max_total_samples{65536};
    std::chrono::nanoseconds max_retained_age{std::chrono::hours(1)};
    std::size_t max_query_results{1024};
    std::chrono::nanoseconds stale_after{std::chrono::seconds(30)};
};

struct MetricStoreTelemetry {
    std::size_t active_series{};
    std::size_t sample_count{};
    std::uint64_t series_evictions{};
    std::uint64_t sample_evictions{};
    std::uint64_t rejected_samples{};
    std::uint64_t stale_query_results{};
};

struct MetricSeriesSnapshot {
    MetricKey key;
    std::vector<MetricSample> samples;
};

class MetricStore {
public:
    explicit MetricStore(const Clock& clock, MetricStoreConfig config = {});

    bool insert(MetricSample sample);
    std::optional<MetricSample> latest(const MetricKey& key) const;
    std::vector<MetricSample> window(const MetricKey& key,
                                     MonotonicTime begin,
                                     MonotonicTime end,
                                     std::size_t limit = 0) const;
    std::vector<MetricSeriesSnapshot> snapshot(
        std::optional<NetnsId> netns = std::nullopt) const;
    MetricStoreTelemetry telemetry() const;
    const MetricStoreConfig& config() const noexcept { return config_; }

private:
    struct Series {
        std::deque<MetricSample> samples;
        MonotonicTime last_updated;
    };
    void pruneExpiredLocked(MonotonicTime now);
    void evictOldestSeriesLocked();
    void evictOldestSampleLocked();
    MetricSample withStaleness(MetricSample sample, MonotonicTime now) const;

    const Clock& clock_;
    MetricStoreConfig config_;
    mutable std::mutex mutex_;
    std::map<MetricKey, Series> series_;
    std::size_t sample_count_{};
    mutable MetricStoreTelemetry counters_;
};

}  // namespace weaknet_dbus::v2
