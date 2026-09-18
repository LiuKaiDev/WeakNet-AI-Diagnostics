#include "metric_store.hpp"

#include <algorithm>
#include <deque>
#include <stdexcept>

namespace weaknet_dbus::v2 {

MetricStore::MetricStore(const Clock& clock, MetricStoreConfig config)
    : clock_(clock), config_(config) {
    if (config_.max_series == 0 || config_.max_samples_per_series == 0 ||
        config_.max_total_samples == 0 || config_.max_query_results == 0 ||
        config_.max_retained_age < std::chrono::nanoseconds::zero() ||
        config_.stale_after < std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument("MetricStore limits must be positive");
    }
}

bool MetricStore::insert(MetricSample sample) {
    if (sample.sample_id.value == 0 || sample.key.netns.inode == 0 ||
        (sample.key.interface && sample.key.interface->ifindex == 0) ||
        (sample.validity == Validity::Valid && !sample.value) ||
        (sample.validity == Validity::Unavailable && sample.value)) {
        std::lock_guard lock(mutex_);
        ++counters_.rejected_samples;
        return false;
    }

    std::lock_guard lock(mutex_);
    pruneExpiredLocked(sample.monotonic_at);
    auto iterator = series_.find(sample.key);
    if (iterator == series_.end()) {
        if (series_.size() == config_.max_series) evictOldestSeriesLocked();
        iterator = series_.emplace(sample.key, Series{}).first;
    }
    auto& series = iterator->second;
    if (!series.samples.empty() && sample.monotonic_at < series.samples.back().monotonic_at) {
        ++counters_.rejected_samples;
        return false;
    }
    series.samples.push_back(std::move(sample));
    series.last_updated = series.samples.back().monotonic_at;
    ++sample_count_;
    while (series.samples.size() > config_.max_samples_per_series) {
        series.samples.pop_front();
        --sample_count_;
        ++counters_.sample_evictions;
    }
    while (sample_count_ > config_.max_total_samples) evictOldestSampleLocked();
    return true;
}

std::optional<MetricSample> MetricStore::latest(const MetricKey& key) const {
    const auto now = clock_.monotonicNow();
    std::lock_guard lock(mutex_);
    const auto iterator = series_.find(key);
    if (iterator == series_.end() || iterator->second.samples.empty()) return std::nullopt;
    if (now - iterator->second.samples.back().monotonic_at > config_.max_retained_age) {
        return std::nullopt;
    }
    return withStaleness(iterator->second.samples.back(), now);
}

std::vector<MetricSample> MetricStore::window(const MetricKey& key,
                                               MonotonicTime begin,
                                               MonotonicTime end,
                                               std::size_t limit) const {
    if (end < begin) return {};
    const auto now = clock_.monotonicNow();
    const auto effective_limit = limit == 0 ? config_.max_query_results
                                             : std::min(limit, config_.max_query_results);
    std::vector<MetricSample> result;
    std::lock_guard lock(mutex_);
    const auto iterator = series_.find(key);
    if (iterator == series_.end()) return result;
    for (const auto& sample : iterator->second.samples) {
        if (sample.monotonic_at < begin || sample.monotonic_at > end ||
            now - sample.monotonic_at > config_.max_retained_age) continue;
        result.push_back(withStaleness(sample, now));
        if (result.size() == effective_limit) break;
    }
    return result;
}

std::vector<MetricSeriesSnapshot> MetricStore::snapshot(std::optional<NetnsId> netns) const {
    const auto now = clock_.monotonicNow();
    std::vector<MetricSeriesSnapshot> result;
    std::lock_guard lock(mutex_);
    result.reserve(series_.size());
    for (const auto& [key, series] : series_) {
        if (netns && key.netns != *netns) continue;
        MetricSeriesSnapshot item;
        item.key = key;
        for (const auto& sample : series.samples) {
            if (now - sample.monotonic_at <= config_.max_retained_age) {
                item.samples.push_back(withStaleness(sample, now));
            }
        }
        if (!item.samples.empty()) result.push_back(std::move(item));
    }
    return result;
}

MetricStoreTelemetry MetricStore::telemetry() const {
    std::lock_guard lock(mutex_);
    auto result = counters_;
    result.active_series = series_.size();
    result.sample_count = sample_count_;
    return result;
}

void MetricStore::pruneExpiredLocked(MonotonicTime now) {
    for (auto iterator = series_.begin(); iterator != series_.end();) {
        auto& samples = iterator->second.samples;
        while (!samples.empty() && now - samples.front().monotonic_at > config_.max_retained_age) {
            samples.pop_front();
            --sample_count_;
            ++counters_.sample_evictions;
        }
        if (samples.empty()) iterator = series_.erase(iterator);
        else ++iterator;
    }
}

void MetricStore::evictOldestSeriesLocked() {
    if (series_.empty()) return;
    auto oldest = series_.begin();
    for (auto iterator = std::next(series_.begin()); iterator != series_.end(); ++iterator) {
        if (iterator->second.last_updated < oldest->second.last_updated) oldest = iterator;
    }
    sample_count_ -= oldest->second.samples.size();
    counters_.sample_evictions += oldest->second.samples.size();
    ++counters_.series_evictions;
    series_.erase(oldest);
}

void MetricStore::evictOldestSampleLocked() {
    if (series_.empty()) return;
    auto oldest = series_.end();
    for (auto iterator = series_.begin(); iterator != series_.end(); ++iterator) {
        if (iterator->second.samples.empty()) continue;
        if (oldest == series_.end() ||
            iterator->second.samples.front().monotonic_at < oldest->second.samples.front().monotonic_at) {
            oldest = iterator;
        }
    }
    if (oldest == series_.end()) return;
    oldest->second.samples.pop_front();
    --sample_count_;
    ++counters_.sample_evictions;
    if (oldest->second.samples.empty()) series_.erase(oldest);
}

MetricSample MetricStore::withStaleness(MetricSample sample, MonotonicTime now) const {
    if ((sample.validity == Validity::Valid || sample.validity == Validity::Partial) &&
        now - sample.monotonic_at > config_.stale_after) {
        sample.validity = Validity::Stale;
        ++counters_.stale_query_results;
    }
    return sample;
}

}  // namespace weaknet_dbus::v2
