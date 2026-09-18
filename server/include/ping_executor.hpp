#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace weaknet_dbus {

class PingExecutor {
public:
    using Completion = std::function<void(int)>;
    using Operation = std::function<int(std::stop_token, const std::string&,
                                        const std::string&, int)>;

    explicit PingExecutor(std::size_t capacity = 8, Operation operation = {},
                          std::string helper_path = {});
    ~PingExecutor();

    PingExecutor(const PingExecutor&) = delete;
    PingExecutor& operator=(const PingExecutor&) = delete;

    bool submit(std::string host, std::string interface_name,
                int timeout_ms, Completion completion);
    void stop() noexcept;
    std::size_t queued() const;

private:
    struct Task {
        std::string host;
        std::string interface_name;
        int timeout_ms = 0;
        Completion completion;
    };

    void run(std::stop_token token);

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<Task> queue_;
    bool accepting_ = true;
    Operation operation_;
    std::jthread worker_;
};

}  // namespace weaknet_dbus
