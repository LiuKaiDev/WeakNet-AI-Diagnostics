#include "socket_tracker.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace weaknet_dbus::v2 {

SocketLifecycleTable::SocketLifecycleTable(SocketLifecycleConfig config)
    : config_(config) {
    if (config_.max_active_entries == 0 || config_.max_closed_entries == 0) {
        throw std::invalid_argument("SocketLifecycleTable bounds must be positive");
    }
}

bool SocketLifecycleTable::validInput(const SocketObservationInput& input) noexcept {
    if (!input.present || input.tuple.netns.inode == 0) return false;
    if (input.tuple.protocol != SocketProtocol::Tcp) return false;
    if (input.tuple.family != input.tuple.local.family ||
        input.tuple.family != input.tuple.remote.family) return false;
    return input.tuple.family == 2 || input.tuple.family == 10;  // AF_INET/AF_INET6
}

SocketGeneration SocketLifecycleTable::nextGeneration() {
    if (next_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("socket lifecycle generation exhausted");
    }
    return SocketGeneration{next_generation_++};
}

SocketId SocketLifecycleTable::newIdentity(
    const SocketTuple& tuple, std::optional<KernelSocketCookie> cookie) {
    return SocketId{tuple.netns, cookie, nextGeneration()};
}

SocketObservation SocketLifecycleTable::makeObservation(
    const SocketId& id, const SocketObservationInput& input) const {
    return SocketObservation{id, input.tuple.netns, input.tuple, input.observed_at, input.monotonic_at,
                             input.source, input.validity, SocketLifecycleState::Active, true};
}

bool SocketLifecycleTable::beginSnapshot(SocketSnapshotDisposition disposition) {
    if (snapshot_in_progress_) return false;
    snapshot_in_progress_ = true;
    disposition_ = disposition;
    pending_.clear();
    pending_weak_.clear();
    pending_cookie_.clear();
    pending_seen_.clear();
    pending_superseded_.clear();
    return true;
}

std::optional<SocketId> SocketLifecycleTable::pendingWeak(const SocketTuple& tuple) const {
    const auto iterator = pending_weak_.find(tuple);
    if (iterator == pending_weak_.end()) return std::nullopt;
    if (pending_superseded_.contains(iterator->second)) return std::nullopt;
    return iterator->second;
}

std::optional<SocketId> SocketLifecycleTable::pendingCookie(const CookieKey& key) const {
    const auto iterator = pending_cookie_.find(key);
    if (iterator == pending_cookie_.end()) return std::nullopt;
    if (pending_superseded_.contains(iterator->second)) return std::nullopt;
    return iterator->second;
}

void SocketLifecycleTable::erasePending(const SocketId& id) {
    pending_.erase(id);
    pending_seen_.erase(id);
    for (auto iterator = pending_weak_.begin(); iterator != pending_weak_.end();) {
        if (iterator->second == id) iterator = pending_weak_.erase(iterator);
        else ++iterator;
    }
    for (auto iterator = pending_cookie_.begin(); iterator != pending_cookie_.end();) {
        if (iterator->second == id) iterator = pending_cookie_.erase(iterator);
        else ++iterator;
    }
}

void SocketLifecycleTable::supersede(const SocketId& id) {
    const bool was_active = active_.contains(id);
    erasePending(id);
    if (was_active) pending_superseded_.insert(id);
}

void SocketLifecycleTable::rememberClosed(SocketObservation observation) {
    observation.lifecycle = SocketLifecycleState::Closed;
    observation.present = false;
    observation.validity = Validity::Stale;
    closed_.push_back(std::move(observation));
    while (closed_.size() > config_.max_closed_entries) closed_.pop_front();
}

std::optional<SocketResolution> SocketLifecycleTable::observe(
    const SocketObservationInput& input) {
    if (!snapshot_in_progress_ || disposition_ == SocketSnapshotDisposition::Failed ||
        !validInput(input)) return std::nullopt;

    std::optional<SocketId> resolved;
    bool inconsistent = false;
    bool created = false;
    if (input.cookie) {
        const CookieKey key{input.tuple.netns, *input.cookie};
        if (const auto candidate = pendingCookie(key)) {
            const auto iterator = pending_.find(*candidate);
            if (iterator != pending_.end() && iterator->second.tuple == input.tuple) {
                resolved = *candidate;
            } else {
                inconsistent = true;
                supersede(*candidate);
            }
        }
        if (!resolved) {
            const auto active = cookie_active_.find(key);
            if (active != cookie_active_.end() && !pending_superseded_.contains(active->second)) {
                const auto iterator = active_.find(active->second);
                if (iterator != active_.end() && iterator->second.tuple == input.tuple) {
                    resolved = active->second;
                } else {
                    inconsistent = true;
                    supersede(active->second);
                }
            }
        }
        if (!resolved) {
            if (const auto weak_pending = pendingWeak(input.tuple)) {
                inconsistent = true;
                supersede(*weak_pending);
            } else {
                const auto weak_active = weak_active_.find(input.tuple);
                if (weak_active != weak_active_.end() &&
                    !pending_superseded_.contains(weak_active->second)) {
                    inconsistent = true;
                    supersede(weak_active->second);
                }
            }
        }
    } else {
        if (const auto weak = pendingWeak(input.tuple)) {
            resolved = *weak;
        } else {
            const auto active = weak_active_.find(input.tuple);
            if (active != weak_active_.end() && !pending_superseded_.contains(active->second)) {
                resolved = active->second;
            } else {
                // Losing cookie visibility is an identity uncertainty. Do
                // not silently merge into a cookie-backed lifecycle; reset it
                // conservatively if the tuple is the only available clue.
                std::optional<SocketId> cookie_pending_to_reset;
                for (const auto& [pending_id, observation] : pending_) {
                    if (pending_id.cookie && observation.tuple == input.tuple &&
                        !pending_superseded_.contains(pending_id)) {
                        inconsistent = true;
                        cookie_pending_to_reset = pending_id;
                        break;
                    }
                }
                if (cookie_pending_to_reset) supersede(*cookie_pending_to_reset);
                std::optional<SocketId> cookie_active_to_reset;
                for (const auto& [active_id, observation] : active_) {
                    if (active_id.cookie && observation.tuple == input.tuple &&
                        !pending_superseded_.contains(active_id)) {
                        inconsistent = true;
                        cookie_active_to_reset = active_id;
                        break;
                    }
                }
                if (cookie_active_to_reset) supersede(*cookie_active_to_reset);
            }
        }
    }

    if (!resolved) {
        if (active_.size() + pending_.size() >= config_.max_active_entries) return std::nullopt;
        resolved = newIdentity(input.tuple, input.cookie);
        created = true;
    }

    const auto observation = makeObservation(*resolved, input);
    pending_[*resolved] = observation;
    pending_seen_.insert(*resolved);
    if (input.cookie) pending_cookie_[CookieKey{input.tuple.netns, *input.cookie}] = *resolved;
    else pending_weak_[input.tuple] = *resolved;
    return SocketResolution{observation, created, inconsistent};
}

bool SocketLifecycleTable::commitSnapshot() {
    if (!snapshot_in_progress_) return false;
    if (disposition_ != SocketSnapshotDisposition::Authoritative) {
        clearCandidate();
        return true;
    }

    std::vector<SocketId> to_close;
    for (const auto& [id, observation] : active_) {
        (void)observation;
        if (!pending_seen_.contains(id) || pending_superseded_.contains(id)) to_close.push_back(id);
    }
    for (const auto& id : to_close) {
        const auto iterator = active_.find(id);
        if (iterator == active_.end()) continue;
        rememberClosed(iterator->second);
        active_.erase(iterator);
    }
    for (const auto& [id, observation] : pending_) {
        if (!pending_superseded_.contains(id)) active_[id] = observation;
    }

    weak_active_.clear();
    cookie_active_.clear();
    for (const auto& [id, observation] : active_) {
        if (id.cookie) cookie_active_[CookieKey{id.netns, *id.cookie}] = id;
        else weak_active_[observation.tuple] = id;
    }
    clearCandidate();
    return true;
}

void SocketLifecycleTable::clearCandidate() noexcept {
    snapshot_in_progress_ = false;
    disposition_ = SocketSnapshotDisposition::Failed;
    pending_.clear();
    pending_weak_.clear();
    pending_cookie_.clear();
    pending_seen_.clear();
    pending_superseded_.clear();
}

void SocketLifecycleTable::abortSnapshot() noexcept { clearCandidate(); }

bool SocketLifecycleTable::close(const SocketId& id) {
    if (snapshot_in_progress_) return false;
    const auto iterator = active_.find(id);
    if (iterator == active_.end()) return false;
    rememberClosed(iterator->second);
    active_.erase(iterator);
    weak_active_.clear();
    cookie_active_.clear();
    for (const auto& [active_id, observation] : active_) {
        if (active_id.cookie) cookie_active_[CookieKey{active_id.netns, *active_id.cookie}] = active_id;
        else weak_active_[observation.tuple] = active_id;
    }
    return true;
}

std::vector<SocketObservation> SocketLifecycleTable::active() const {
    std::vector<SocketObservation> result;
    result.reserve(active_.size());
    for (const auto& [id, observation] : active_) {
        (void)id;
        result.push_back(observation);
    }
    return result;
}

std::vector<SocketObservation> SocketLifecycleTable::recentlyClosed() const {
    return {closed_.begin(), closed_.end()};
}

}  // namespace weaknet_dbus::v2
