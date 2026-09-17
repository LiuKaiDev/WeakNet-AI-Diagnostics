#include "common.hpp"

#include <dbus/dbus.h>

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace {

struct Member {
    std::string input;
    std::string output;

    bool operator==(const Member&) const = default;
};

struct Contract {
    std::string bus;
    std::string path;
    std::string interface;
    std::string error;
    std::map<std::string, Member> methods;
    std::map<std::string, std::string> signals;
};

bool splitOnce(const std::string& value, char separator,
               std::string& left, std::string& right) {
    const auto position = value.find(separator);
    if (position == std::string::npos) return false;
    left = value.substr(0, position);
    right = value.substr(position + 1);
    return true;
}

bool parseContract(const std::string& path, Contract& contract) {
    std::ifstream input(path);
    if (!input) return false;

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') continue;
        std::string key;
        std::string value;
        if (!splitOnce(line, '=', key, value)) return false;
        if (key == "bus") contract.bus = value;
        else if (key == "path") contract.path = value;
        else if (key == "interface") contract.interface = value;
        else if (key == "error") contract.error = value;
        else if (key == "method") {
            std::string name;
            std::string signatures;
            if (!splitOnce(value, '|', name, signatures)) return false;
            std::string input_signature;
            std::string output_signature;
            if (!splitOnce(signatures, '|', input_signature, output_signature)) return false;
            if (!contract.methods.emplace(name, Member{input_signature, output_signature}).second) {
                return false;
            }
        } else if (key == "signal") {
            std::string name;
            std::string signature;
            if (!splitOnce(value, '|', name, signature)) return false;
            if (!contract.signals.emplace(name, signature).second) return false;
        } else {
            return false;
        }
    }
    return true;
}

bool appendSignature(DBusMessage* message, const std::string& signature) {
    DBusMessageIter root;
    dbus_message_iter_init_append(message, &root);
    for (char type : signature) {
        if (type == 's') {
            const char* value = "fixture";
            if (!dbus_message_iter_append_basic(&root, DBUS_TYPE_STRING, &value)) return false;
        } else if (type == 'i') {
            dbus_int32_t value = 7;
            if (!dbus_message_iter_append_basic(&root, DBUS_TYPE_INT32, &value)) return false;
        } else if (type == 'a') {
            return false;
        } else {
            return false;
        }
    }
    return true;
}

bool appendOutputSignature(DBusMessage* message, const std::string& signature) {
    if (signature == "as") {
        DBusMessageIter root;
        DBusMessageIter array;
        dbus_message_iter_init_append(message, &root);
        if (!dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY,
                                               DBUS_TYPE_STRING_AS_STRING, &array)) {
            return false;
        }
        const char* value = "eth0";
        if (!dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &value)) return false;
        return dbus_message_iter_close_container(&root, &array);
    }
    return appendSignature(message, signature);
}

bool expect(bool condition, const std::string& message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: v1_dbus_contract <fixture>\n";
        return 2;
    }

    Contract contract;
    bool ok = expect(parseContract(argv[1], contract), "parse contract fixture");
    ok &= expect(contract.bus == weaknet_dbus::kBusName, "bus name");
    ok &= expect(contract.path == weaknet_dbus::kObjectPath, "object path");
    ok &= expect(contract.interface == weaknet_dbus::kInterface, "interface name");
    ok &= expect(contract.error == "com.example.WeakNet.Error", "error name");

    const std::map<std::string, Member> expected_methods{
        {weaknet_dbus::kMethodGet, {"", "s"}},
        {weaknet_dbus::kMethodListInterfaces, {"", "as"}},
        {weaknet_dbus::kMethodGetInterfaces, {"", "as"}},
        {weaknet_dbus::kMethodHealthCheck, {"", "s"}},
        {weaknet_dbus::kMethodPing, {"s", "s"}},
    };
    const std::map<std::string, std::string> expected_signals{
        {weaknet_dbus::kSignalChanged, "si"},
        {weaknet_dbus::kSignalInterfaceChanged, "si"},
        {weaknet_dbus::kSignalConnectionModeChanged, "si"},
        {weaknet_dbus::kSignalNetworkQualityChanged, "ssi"},
    };
    ok &= expect(contract.methods == expected_methods, "method set and signatures");
    ok &= expect(contract.signals == expected_signals, "signal set and signatures");

    for (const auto& [name, member] : contract.methods) {
        DBusMessage* call = dbus_message_new_method_call(
            contract.bus.c_str(), contract.path.c_str(), contract.interface.c_str(), name.c_str());
        ok &= expect(call != nullptr, "create method call " + name);
        if (!call) continue;
        ok &= expect(appendSignature(call, member.input), "append method input " + name);
        ok &= expect(std::string(dbus_message_get_signature(call)) == member.input,
                     "method input signature " + name);
        dbus_message_set_serial(call, 1);
        DBusMessage* reply = dbus_message_new_method_return(call);
        ok &= expect(reply != nullptr, "create method reply " + name);
        if (reply) {
            ok &= expect(appendOutputSignature(reply, member.output),
                         "append method output " + name);
            ok &= expect(std::string(dbus_message_get_signature(reply)) == member.output,
                         "method output signature " + name);
            dbus_message_unref(reply);
        }
        dbus_message_unref(call);
    }

    for (const auto& [name, signature] : contract.signals) {
        DBusMessage* signal = dbus_message_new_signal(
            contract.path.c_str(), contract.interface.c_str(), name.c_str());
        ok &= expect(signal != nullptr, "create signal " + name);
        if (!signal) continue;
        ok &= expect(appendSignature(signal, signature), "append signal " + name);
        ok &= expect(std::string(dbus_message_get_signature(signal)) == signature,
                     "signal signature " + name);
        dbus_message_unref(signal);
    }

    return ok ? 0 : 1;
}
