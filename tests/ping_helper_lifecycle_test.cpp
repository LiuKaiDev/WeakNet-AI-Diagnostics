#include "ping_executor.hpp"

#include <cerrno>
#include <chrono>
#include <iostream>
#include <sys/wait.h>
#include <thread>

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    weaknet_dbus::PingExecutor executor(1, {}, argv[1]);
    if (!executor.submit("host", "interface", 3000, [](int) {})) return 1;
    std::this_thread::sleep_for(150ms);
    const auto started = std::chrono::steady_clock::now();
    executor.stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    std::cout << "ping-helper-stop-ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
              << '\n';
    if (elapsed >= 1s) return 1;

    int status = 0;
    errno = 0;
    if (::waitpid(-1, &status, WNOHANG) != -1 || errno != ECHILD) {
        std::cerr << "Ping helper child was not reaped\n";
        return 1;
    }
    return 0;
}
