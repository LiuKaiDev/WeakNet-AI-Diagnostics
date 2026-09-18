#include <dbus/dbus.h>

#include "looper.hpp"

namespace weaknet_dbus {

void Looper::run(std::stop_token token) {
    while (!token.stop_requested() && dbus_connection_get_is_connected(connection_)) {
        dbus_connection_read_write_dispatch(connection_, 100);
    }
}

}  // namespace weaknet_dbus
