#include "ping_executor.hpp"

#include <utility>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <poll.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "net_ping.h"
#include "scoped_fd.hpp"

extern char** environ;

namespace weaknet_dbus {
namespace {

int runPingHelper(std::stop_token token, const std::string& helper_path,
                  const std::string& host, const std::string& interface_name,
                  int timeout_ms) {
    int pipe_descriptors[2]{};
    if (::pipe2(pipe_descriptors, O_CLOEXEC) != 0) return -10;
    ScopedFd read_end(pipe_descriptors[0]);
    ScopedFd write_end(pipe_descriptors[1]);

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) return -10;
    posix_spawn_file_actions_adddup2(&actions, write_end.get(), STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, read_end.get());
    posix_spawn_file_actions_addclose(&actions, write_end.get());

    const std::string timeout = std::to_string(timeout_ms);
    char* const arguments[] = {
        const_cast<char*>(helper_path.c_str()),
        const_cast<char*>(host.c_str()),
        const_cast<char*>(interface_name.c_str()),
        const_cast<char*>(timeout.c_str()),
        nullptr,
    };
    pid_t child = -1;
    const int spawn_result = posix_spawn(
        &child, helper_path.c_str(), &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    write_end.reset();
    if (spawn_result != 0) return -10;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms + 1000);
    std::string output;
    bool child_exited = false;
    int status = 0;
    while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        pollfd descriptor{read_end.get(), POLLIN | POLLHUP, 0};
        const int poll_result = ::poll(&descriptor, 1, 50);
        if (poll_result > 0 && (descriptor.revents & (POLLIN | POLLHUP))) {
            char buffer[64];
            const ssize_t count = ::read(read_end.get(), buffer, sizeof(buffer));
            if (count > 0) output.append(buffer, static_cast<std::size_t>(count));
        }
        const pid_t wait_result = ::waitpid(child, &status, WNOHANG);
        if (wait_result == child) {
            child_exited = true;
            break;
        }
        if (wait_result < 0 && errno != EINTR) break;
    }

    if (!child_exited) {
        ::kill(child, SIGKILL);
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        return token.stop_requested() ? -11 : -5;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -10;
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(output.c_str(), &end, 10);
    if (errno != 0 || end == output.c_str()) return -10;
    return static_cast<int>(value);
}

}  // namespace

PingExecutor::PingExecutor(std::size_t capacity, Operation operation,
                           std::string helper_path)
    : capacity_(capacity), operation_(std::move(operation)) {
    if (!operation_) {
        if (!helper_path.empty()) {
            operation_ = [path = std::move(helper_path)](
                std::stop_token token, const std::string& host,
                const std::string& interface_name, int timeout_ms) {
                return runPingHelper(token, path, host, interface_name, timeout_ms);
            };
        } else {
            operation_ = [](std::stop_token, const std::string& host,
                            const std::string& interface_name, int timeout_ms) {
                return NetPing::getInstance()->ping(host, interface_name, timeout_ms);
            };
        }
    }
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
}

PingExecutor::~PingExecutor() {
    stop();
}

bool PingExecutor::submit(std::string host, std::string interface_name,
                          int timeout_ms, Completion completion) {
    std::lock_guard lock(mutex_);
    if (!accepting_ || queue_.size() >= capacity_) return false;
    queue_.push_back(Task{std::move(host), std::move(interface_name),
                          timeout_ms, std::move(completion)});
    condition_.notify_one();
    return true;
}

void PingExecutor::stop() noexcept {
    {
        std::lock_guard lock(mutex_);
        accepting_ = false;
        queue_.clear();
    }
    worker_.request_stop();
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::size_t PingExecutor::queued() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

void PingExecutor::run(std::stop_token token) {
    while (!token.stop_requested()) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, token, [this] { return !queue_.empty() || !accepting_; });
            if (token.stop_requested() || (!accepting_ && queue_.empty())) break;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        const int result = operation_(token, task.host, task.interface_name,
                                      task.timeout_ms);
        if (!token.stop_requested() && task.completion) task.completion(result);
    }
}

}  // namespace weaknet_dbus
