#include "event_bus.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace weaknet_dbus::v2 {

bool EventFilter::matches(const NetworkEvent& event) const noexcept {
    const auto& header = event.header();
    return (!kind || *kind == header.kind) &&
           (!source || *source == header.source) &&
           (!netns || *netns == header.netns);
}

struct EventBus::State : std::enable_shared_from_this<EventBus::State> {
    struct Subscriber {
        EventFilter filter;
        Callback callback;
        mutable std::mutex mutex;
        std::condition_variable cv;
        bool active{true};
        std::size_t in_flight{};
        std::thread::id executing_thread;
    };

    explicit State(std::size_t configured_capacity) : capacity(configured_capacity) {
        if (capacity == 0) throw std::invalid_argument("EventBus capacity must be positive");
        counters.queue_capacity = capacity;
    }

    bool start() {
        std::lock_guard lock(queue_mutex);
        if (started && !stopping) return true;
        if (started || stopped) return false;
        started = true;
        dispatcher = std::jthread([self = shared_from_this()] { self->dispatchLoop(); });
        return true;
    }

    void stop() noexcept {
        std::unique_lock lock(queue_mutex);
        if (!started || joined) return;
        if (!stopping) {
            stopping = true;
            queue_cv.notify_all();
        }
        if (std::this_thread::get_id() == dispatcher.get_id()) return;
        if (join_claimed) {
            queue_cv.wait(lock, [this] { return joined; });
            return;
        }
        join_claimed = true;
        lock.unlock();
        if (dispatcher.joinable()) dispatcher.join();
        lock.lock();
        joined = true;
        queue_cv.notify_all();
    }

    PublishResult publish(NetworkEvent event) {
        std::lock_guard lock(queue_mutex);
        const auto kind_index = static_cast<std::size_t>(event.header().kind);
        const auto source_index = static_cast<std::size_t>(event.header().source);
        if (!started || stopping || stopped || next_sequence == 0) {
            ++counters.total_drops;
            ++counters.dropped_by_kind.at(kind_index);
            return {};
        }

        bool coalesced = false;
        if (queue.size() == capacity) {
            if (event.replaceable()) {
                const auto key = event.coalescingKey();
                const auto existing = std::find_if(queue.begin(), queue.end(), [&key](const NetworkEvent& queued) {
                    return queued.replaceable() && queued.coalescingKey() == key;
                });
                if (existing != queue.end()) {
                    queue.erase(existing);
                    ++counters.total_coalesces;
                    coalesced = true;
                }
            }
            if (!coalesced) {
                ++counters.total_drops;
                ++counters.dropped_by_kind.at(kind_index);
                return {};
            }
        }

        const EventSequence assigned{next_sequence++};
        queue.push_back(event.withSequence(assigned));
        ++counters.accepted_publications;
        ++counters.accepted_by_kind.at(kind_index);
        ++counters.accepted_by_source.at(source_index);
        counters.high_water_mark = std::max(counters.high_water_mark, queue.size());
        queue_cv.notify_one();
        return {true, coalesced, assigned};
    }

    std::uint64_t subscribe(EventFilter filter, Callback callback) {
        if (!callback) throw std::invalid_argument("EventBus callback must be callable");
        auto subscriber = std::make_shared<Subscriber>();
        subscriber->filter = std::move(filter);
        subscriber->callback = std::move(callback);
        std::lock_guard lock(registry_mutex);
        const auto id = next_subscriber++;
        subscribers.emplace(id, std::move(subscriber));
        return id;
    }

    void unsubscribe(std::uint64_t id) noexcept {
        if (id == 0) return;
        std::shared_ptr<Subscriber> subscriber;
        {
            std::lock_guard lock(registry_mutex);
            const auto iterator = subscribers.find(id);
            if (iterator == subscribers.end()) return;
            subscriber = iterator->second;
            subscribers.erase(iterator);
        }
        std::unique_lock lock(subscriber->mutex);
        subscriber->active = false;
        if (std::this_thread::get_id() != subscriber->executing_thread) {
            subscriber->cv.wait(lock, [&subscriber] { return subscriber->in_flight == 0; });
        }
    }

    bool subscriptionActive(std::uint64_t id) const noexcept {
        if (id == 0) return false;
        std::lock_guard lock(registry_mutex);
        return subscribers.contains(id);
    }

    EventBusTelemetry telemetry() const {
        std::lock_guard lock(queue_mutex);
        auto result = counters;
        result.queue_depth = queue.size();
        return result;
    }

    bool running() const noexcept {
        std::lock_guard lock(queue_mutex);
        return started && !stopping && !stopped;
    }

    void dispatchLoop() noexcept {
        while (true) {
            std::optional<NetworkEvent> event;
            {
                std::unique_lock lock(queue_mutex);
                queue_cv.wait(lock, [this] { return stopping || !queue.empty(); });
                if (queue.empty() && stopping) break;
                event.emplace(std::move(queue.front()));
                queue.pop_front();
            }

            std::vector<std::shared_ptr<Subscriber>> targets;
            {
                std::lock_guard lock(registry_mutex);
                targets.reserve(subscribers.size());
                for (const auto& [id, subscriber] : subscribers) {
                    (void)id;
                    if (subscriber->filter.matches(*event)) targets.push_back(subscriber);
                }
            }
            for (const auto& subscriber : targets) {
                {
                    std::lock_guard lock(subscriber->mutex);
                    if (!subscriber->active) continue;
                    ++subscriber->in_flight;
                    subscriber->executing_thread = std::this_thread::get_id();
                }
                try {
                    subscriber->callback(*event);
                } catch (...) {
                    std::lock_guard lock(queue_mutex);
                    ++counters.callback_failures;
                }
                {
                    std::lock_guard lock(subscriber->mutex);
                    --subscriber->in_flight;
                    subscriber->executing_thread = {};
                    subscriber->cv.notify_all();
                }
            }
            {
                std::lock_guard lock(queue_mutex);
                ++counters.dispatched_publications;
            }
        }
        {
            std::lock_guard lock(queue_mutex);
            stopped = true;
        }
        queue_cv.notify_all();
    }

    const std::size_t capacity;
    mutable std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<NetworkEvent> queue;
    EventBusTelemetry counters;
    std::uint64_t next_sequence{1};
    bool started{false};
    bool stopping{false};
    bool stopped{false};
    bool join_claimed{false};
    bool joined{false};
    std::jthread dispatcher;
    mutable std::mutex registry_mutex;
    std::map<std::uint64_t, std::shared_ptr<Subscriber>> subscribers;
    std::uint64_t next_subscriber{1};
};

EventBus::Subscription::Subscription(std::weak_ptr<State> state, std::uint64_t id) noexcept
    : state_(std::move(state)), id_(id) {}

EventBus::Subscription::~Subscription() { unsubscribe(); }

EventBus::Subscription::Subscription(Subscription&& other) noexcept
    : state_(std::move(other.state_)), id_(std::exchange(other.id_, 0)) {}

EventBus::Subscription& EventBus::Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        unsubscribe();
        state_ = std::move(other.state_);
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

void EventBus::Subscription::unsubscribe() noexcept {
    const auto id = std::exchange(id_, 0);
    if (auto state = state_.lock()) state->unsubscribe(id);
    state_.reset();
}

bool EventBus::Subscription::active() const noexcept {
    if (auto state = state_.lock()) return state->subscriptionActive(id_);
    return false;
}

EventBus::EventBus(std::size_t capacity) : state_(std::make_shared<State>(capacity)) {}
EventBus::~EventBus() { stop(); }
bool EventBus::start() { return state_->start(); }
void EventBus::stop() noexcept { state_->stop(); }
PublishResult EventBus::publish(NetworkEvent event) { return state_->publish(std::move(event)); }
EventBus::Subscription EventBus::subscribe(EventFilter filter, Callback callback) {
    return Subscription(state_, state_->subscribe(std::move(filter), std::move(callback)));
}
EventBusTelemetry EventBus::telemetry() const { return state_->telemetry(); }
bool EventBus::running() const noexcept { return state_->running(); }

}  // namespace weaknet_dbus::v2
