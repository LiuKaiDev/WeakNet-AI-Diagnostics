#include "network_quality_assessor.hpp"

#include <cmath>
#include <glog/logging.h>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argc > 0 ? argv[0] : "network_quality_baseline");
    using namespace weaknet_dbus;

    bool ok = true;
    NetworkQualityAssessor assessor;
    const auto empty = assessor.assessQuality({});
    ok &= expect(empty.level == NetworkQualityLevel::UNKNOWN,
                 "empty interface set is unknown");
    ok &= expect(empty.score == 0.0, "empty interface score is zero");
    ok &= expect(empty.details == "{\"error\":\"No interfaces available\"}",
                 "empty interface details are stable");

    NetInfo interface("test0");
    interface.setUsingNow(true);
    interface.setState(NetState::Up);
    interface.setRttMs(25);
    interface.setTcpLossRate(0.05);
    interface.setRssiDbm(-55);

    const auto measured = assessor.assessQuality({interface});
    ok &= expect(measured.level == NetworkQualityLevel::GOOD,
                 "known baseline is GOOD");
    ok &= expect(std::abs(measured.score - 86.0) < 0.001,
                 "known baseline score is stable");
    ok &= expect(measured.details.find("\"interface\":\"test0\"") != std::string::npos,
                 "details contain interface");
    ok &= expect(measured.details.find("\"quality_score\":86.0") != std::string::npos,
                 "details contain baseline score");

    google::ShutdownGoogleLogging();
    return ok ? 0 : 1;
}
