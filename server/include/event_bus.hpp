#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "network_event.hpp"

namespace weaknet_dbus::v2 {

struct EventFilter {
    std::optional<EventKind> kind;
    std::optional<EventSource> source;
    std::optional<NetnsId> netns;

    bool matches(const NetworkEvent& event) const noexcept;
};

struct EventBusTelemetry {
    std::size_t queue_capacity{};
    std::size_t queue_depth{};
    std::size_t high_water_mark{};
    std::uint64_t accepted_publications{};
    std::uint64_t dispatched_publications{};
    std::uint64_t total_drops{};
    std::uint64_t total_coalesces{};
    std::uint64_t callback_failures{};
    std::array<std::uint64_t, 7> accepted_by_kind{};
    std::array<std::uint64_t, 7> dropped_by_kind{};
    std::array<std::uint64_t, 7> accepted_by_source{};
};

struct PublishResult {
    bool accepted{false};
    bool coalesced{false};
    EventSequence sequence{};
};

class EventBus {
    struct State;

public:
    using Callback = std::function<void(const NetworkEvent&)>;

    class Subscription {
    public:
        Subscription() = default;
        ~Subscription();
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        void unsubscribe() noexcept;
        bool active() const noexcept;

    private:
        friend class EventBus;
        Subscription(std::weak_ptr<State> state, std::uint64_t id) noexcept;
        std::weak_ptr<State> state_;
        std::uint64_t id_{};
    };

    explicit EventBus(std::size_t capacity = 1024);
    ~EventBus();
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    bool start();
    void stop() noexcept;
    PublishResult publish(NetworkEvent event);
    Subscription subscribe(EventFilter filter, Callback callback);
    EventBusTelemetry telemetry() const;
    bool running() const noexcept;

private:
    std::shared_ptr<State> state_;
};

}  // namespace weaknet_dbus::v2
