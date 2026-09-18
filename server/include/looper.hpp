#pragma once

#include <stop_token>

struct DBusConnection;

namespace weaknet_dbus {

class Looper {
public:
    explicit Looper(DBusConnection* connection) : connection_(connection) {}
    void run(std::stop_token token);

private:
    DBusConnection* connection_ = nullptr;
};

}  // namespace weaknet_dbus
