#include <dbus/dbus.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

constexpr const char* kBusName = "com.example.WeakNet";
constexpr const char* kObjectPath = "/com/example/WeakNet";
constexpr const char* kInterface = "com.example.WeakNet";

bool expect(bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

pid_t launch(const char* executable, const std::filesystem::path& root) {
    const pid_t child = ::fork();
    if (child != 0) return child;
    ::setenv("WEAKNET_STATE_DIR", (root / "state").c_str(), 1);
    ::setenv("WEAKNET_LOG_DIR", (root / "log").c_str(), 1);
    ::setenv("WEAKNET_RUNTIME_DIR", (root / "run").c_str(), 1);
    if (::chdir("/") != 0) _exit(126);
    ::execl(executable, executable, static_cast<char*>(nullptr));
    _exit(127);
}

bool waitForOwner(DBusConnection* connection, bool expected, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        DBusError error;
        dbus_error_init(&error);
        const bool owner = dbus_bus_name_has_owner(connection, kBusName, &error);
        if (dbus_error_is_set(&error)) dbus_error_free(&error);
        if (owner == expected) return true;
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

bool stopAndWait(pid_t child, int signal, const char* label) {
    const auto started = std::chrono::steady_clock::now();
    if (::kill(child, signal) != 0) return false;
    int status = 0;
    const auto deadline = started + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result == child) {
            const auto elapsed = std::chrono::steady_clock::now() - started;
            std::cout << label << "-shutdown-ms="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                      << '\n';
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        std::this_thread::sleep_for(10ms);
    }
    ::kill(child, SIGKILL);
    ::waitpid(child, &status, 0);
    return false;
}

bool sendPing(DBusConnection* connection) {
    DBusMessage* message = dbus_message_new_method_call(
        kBusName, kObjectPath, kInterface, "Ping");
    if (!message) return false;
    const char* host = "192.0.2.1";
    if (!dbus_message_append_args(message, DBUS_TYPE_STRING, &host, DBUS_TYPE_INVALID)) {
        dbus_message_unref(message);
        return false;
    }
    DBusPendingCall* pending = nullptr;
    const bool sent = dbus_connection_send_with_reply(connection, message, &pending, 10000);
    dbus_connection_flush(connection);
    dbus_message_unref(message);
    if (pending) dbus_pending_call_unref(pending);
    return sent;
}

bool degradedHealthVisible(DBusConnection* connection) {
    DBusMessage* message = dbus_message_new_method_call(
        kBusName, kObjectPath, kInterface, "HealthCheck");
    if (!message) return false;
    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        connection, message, 3000, &error);
    dbus_message_unref(message);
    if (!reply || dbus_error_is_set(&error)) {
        if (dbus_error_is_set(&error)) dbus_error_free(&error);
        if (reply) dbus_message_unref(reply);
        return false;
    }
    const char* text = nullptr;
    const bool parsed = dbus_message_get_args(
        reply, &error, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
    const std::string value = parsed && text ? text : "";
    if (dbus_error_is_set(&error)) dbus_error_free(&error);
    dbus_message_unref(reply);
    return value.find("\"runtime_health\"") != std::string::npos &&
           value.find("\"overall\":\"degraded\"") != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: daemon_process_test <daemon> <fixture>\n";
        return 1;
    }
    bool ok = true;
    char temporary_template[] = "/tmp/weaknet-process-test-XXXXXX";
    char* temporary = ::mkdtemp(temporary_template);
    if (!temporary) return 1;
    const std::filesystem::path root(temporary);

    DBusError error;
    dbus_error_init(&error);
    DBusConnection* connection = dbus_bus_get(DBUS_BUS_SESSION, &error);
    if (!connection || dbus_error_is_set(&error)) return 1;

    pid_t term_child = launch(argv[1], root / "term");
    ok &= expect(waitForOwner(connection, true, 5s), "SIGTERM daemon did not acquire name");
    ok &= expect(stopAndWait(term_child, SIGTERM, "sigterm"), "SIGTERM shutdown failed");
    ok &= expect(waitForOwner(connection, false, 5s), "D-Bus name remained after SIGTERM");

    pid_t int_child = launch(argv[1], root / "int");
    ok &= expect(waitForOwner(connection, true, 5s), "SIGINT daemon did not acquire name");
    ok &= expect(stopAndWait(int_child, SIGINT, "sigint"), "SIGINT shutdown failed");
    ok &= expect(waitForOwner(connection, false, 5s), "D-Bus name remained after SIGINT");

    pid_t active_child = launch(argv[2], root / "active");
    ok &= expect(waitForOwner(connection, true, 5s), "active fixture did not acquire name");
    ok &= expect(degradedHealthVisible(connection), "degraded runtime health was not visible");
    ok &= expect(sendPing(connection), "could not queue active Ping request");
    std::this_thread::sleep_for(200ms);
    ok &= expect(stopAndWait(active_child, SIGTERM, "active-ping"),
                 "shutdown during active Ping failed");
    ok &= expect(waitForOwner(connection, false, 5s), "D-Bus name remained after active shutdown");

    dbus_connection_unref(connection);
    std::filesystem::remove_all(root);
    return ok ? 0 : 1;
}
