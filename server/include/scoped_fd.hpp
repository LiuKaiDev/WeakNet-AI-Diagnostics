#pragma once

#include <unistd.h>

namespace weaknet_dbus {

class ScopedFd {
public:
    ScopedFd() = default;
    explicit ScopedFd(int fd) noexcept : fd_(fd) {}
    ~ScopedFd() { reset(); }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept : fd_(other.release()) {}
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }

    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void reset(int replacement = -1) noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = replacement;
    }

private:
    int fd_ = -1;
};

}  // namespace weaknet_dbus
