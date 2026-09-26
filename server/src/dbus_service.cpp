// dbus_service.cpp
// 实现 DBus 服务类：方法处理与信号发送

#include <dbus/dbus.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include "logger.hpp"

#include "common.hpp"
#include "serializer.hpp"
#include "server.hpp"
#include "dbus_service.hpp"
#include "weak_netmgr.hpp"
#include "net_info.hpp"
#include "network_quality_assessor.hpp"
#include "net_ping.h"
#include "ping_executor.hpp"
#include "runtime_config.hpp"
#include "runtime_health.hpp"
#include "diagnostics_query.hpp"
#include "common.hpp"

#include <sstream>
#include <type_traits>
#include <functional>
#include <chrono>

namespace weaknet_dbus {

DbusService::DbusService(ServerContext* ctx, PingExecutor::Operation ping_operation,
                         std::string ping_helper_path)
    : ctx_(ctx),
      ping_executor_(std::make_unique<PingExecutor>(
          8, std::move(ping_operation), std::move(ping_helper_path))) {}

DbusService::~DbusService() {
    beginShutdown();
}

void DbusService::beginShutdown() noexcept {
    if (ping_executor_) ping_executor_->stop();
}

// 静态自由函数，转调到对象实例
static DBusHandlerResult MessageHandlerStatic(DBusConnection* conn, DBusMessage* msg, void* user_data) {
    auto* self = reinterpret_cast<DbusService*>(user_data);
    if (!self) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (dbus_message_is_method_call(msg, kInterface, kMethodGet)) {
        self->handleGet(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodListInterfaces)) {
        self->handleListInterfaces(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetInterfaces)) {
        self->handleListInterfaces(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodHealthCheck)) {
        self->handleHealthCheck(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodPing)) {
        self->handlePing(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult V2MessageHandlerStatic(DBusConnection* conn, DBusMessage* msg, void* user_data) {
    auto* self = reinterpret_cast<DbusService*>(user_data);
    if (!self) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (dbus_message_is_method_call(msg, kV2Interface, kV2MethodGetStatus))
        self->handleV2GetStatus(conn, msg);
    else if (dbus_message_is_method_call(msg, kV2Interface, kV2MethodListActiveIncidents))
        self->handleV2ListIncidents(conn, msg);
    else if (dbus_message_is_method_call(msg, kV2Interface, kV2MethodListRootCauseHypotheses))
        self->handleV2ListHypotheses(conn, msg);
    else if (dbus_message_is_method_call(msg, kV2Interface, kV2MethodGetDiagnosis))
        self->handleV2GetDiagnosis(conn, msg);
    else if (dbus_message_is_method_call(msg, kV2Interface, kV2MethodGetTopologySummary))
        self->handleV2GetTopology(conn, msg);
    else return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    return DBUS_HANDLER_RESULT_HANDLED;
}

bool DbusService::register_on_connection(DBusConnection* conn) {
    static DBusObjectPathVTable vtable{};
    vtable.message_function = &MessageHandlerStatic;
    if (!dbus_connection_register_object_path(conn, kObjectPath, &vtable, this)) return false;
    static DBusObjectPathVTable v2_vtable{};
    v2_vtable.message_function = &V2MessageHandlerStatic;
    if (!dbus_connection_register_object_path(conn, kV2ObjectPath, &v2_vtable, this)) {
        dbus_connection_unregister_object_path(conn, kObjectPath);
        return false;
    }
    return true;
}

bool DbusService::emitChanged(const std::string& message, int32_t counter) {
    std::lock_guard lock(output_mutex_);
    DBusMessage* sig = dbus_message_new_signal(kObjectPath, kInterface, kSignalChanged);
    if (!sig) return false;
    DBusMessageIter args;
    dbus_message_iter_init_append(sig, &args);
    const char* s = message.c_str();
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s)) { dbus_message_unref(sig); return false; }
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &counter)) { dbus_message_unref(sig); return false; }
    bool ok = dbus_connection_send(ctx_->connection, sig, nullptr);
    dbus_message_unref(sig);
    ChangedPayload payload{message, counter};
    std::string err;
    if (ctx_->config) {
        serializeChangedPayloadToFile(payload, ctx_->config->signalFile().string(), &err);
    }
    return ok;
}

// MessageHandler 实现已移动到静态自由函数

bool DbusService::handleGet(DBusConnection* conn, DBusMessage* msg) {
    std::lock_guard lock(output_mutex_);
    const char* reply_text = "Hello from WeakNet Server";
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;
    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = reply_text;
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s)) { dbus_message_unref(reply); return false; }
    if (!dbus_connection_send(conn, reply, nullptr)) { dbus_message_unref(reply); return false; }
    dbus_message_unref(reply);
    std::string err;
    if (ctx_ && ctx_->config) {
        serializeGetReplyToFile(reply_text, ctx_->config->getReplyFile().string(), &err);
    }
    return true;
}

bool DbusService::replyStringArray(DBusConnection* conn, DBusMessage* msg, const std::vector<std::string>& arr) {
    std::lock_guard lock(output_mutex_);
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;
    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);
    DBusMessageIter array_iter;
    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &array_iter)) { dbus_message_unref(reply); return false; }
    for (const auto& s : arr) {
        const char* cs = s.c_str();
        if (!dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_STRING, &cs)) { dbus_message_iter_close_container(&iter, &array_iter); dbus_message_unref(reply); return false; }
    }
    if (!dbus_message_iter_close_container(&iter, &array_iter)) { dbus_message_unref(reply); return false; }
    bool ok = dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
    return ok;
}

bool DbusService::handleListInterfaces(DBusConnection* conn, DBusMessage* msg) {
    std::vector<NetInfo> ifaces;
    if (ctx_ && ctx_->weak_mgr) {
        ifaces = ctx_->weak_mgr->getCurrentInterfaces();
    }
    std::vector<std::string> snapshot = WeakNetMgr::namesOf(ifaces);
    return replyStringArray(conn, msg, snapshot);
}

bool DbusService::handleHealthCheck(DBusConnection* conn, DBusMessage* msg) {
    std::vector<NetInfo> snapshot;
    if (ctx_ && ctx_->weak_mgr) {
        snapshot = ctx_->weak_mgr->getCurrentInterfaces();
    }

    NetworkQualityAssessor assessor;
    NetworkQualityResult result = assessor.assessQuality(snapshot);
    std::string reply_text = result.details;
    if (ctx_ && ctx_->health && !reply_text.empty() && reply_text.back() == '}') {
        reply_text.pop_back();
        reply_text += ",\"runtime_health\":" + ctx_->health->toJson() + "}";
    }

    std::lock_guard lock(output_mutex_);
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;
    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = reply_text.c_str();
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s)) { dbus_message_unref(reply); return false; }
    if (!dbus_connection_send(conn, reply, nullptr)) { dbus_message_unref(reply); return false; }
    dbus_message_unref(reply);
    return true;
}

bool DbusService::emitSpecificSignal(const std::string& signalName, const std::string& message, int32_t counter) {
    if (!ctx_ || !ctx_->connection) return false;

    std::lock_guard lock(output_mutex_);
    DBusMessage* signal = dbus_message_new_signal(kObjectPath, kInterface, signalName.c_str());
    if (!signal) return false;

    DBusMessageIter iter;
    dbus_message_iter_init_append(signal, &iter);

    const char* msg = message.c_str();
    if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &msg)) {
        dbus_message_unref(signal);
        return false;
    }

    if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &counter)) {
        dbus_message_unref(signal);
        return false;
    }

    bool ok = dbus_connection_send(ctx_->connection, signal, nullptr);
    dbus_message_unref(signal);
    
    LOG_INFO(LogModule::DBUS, "emitted signal: " << signalName << ", message='" << message << "', counter=" << counter);
    return ok;
}

bool DbusService::emitNetworkQualitySignal(const std::string& message, const std::string& details, int32_t counter) {
    if (!ctx_ || !ctx_->connection) return false;

    std::lock_guard lock(output_mutex_);
    DBusMessage* signal = dbus_message_new_signal(kObjectPath, kInterface, kSignalNetworkQualityChanged);
    if (!signal) return false;

    DBusMessageIter iter;
    dbus_message_iter_init_append(signal, &iter);

    // 添加质量等级参数
    const char* quality = message.c_str();
    if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &quality)) {
        dbus_message_unref(signal);
        return false;
    }

    // 添加详细信息参数
    const char* details_str = details.c_str();
    if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &details_str)) {
        dbus_message_unref(signal);
        return false;
    }

    // 添加计数器参数
    if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &counter)) {
        dbus_message_unref(signal);
        return false;
    }

    bool ok = dbus_connection_send(ctx_->connection, signal, nullptr);
    dbus_message_unref(signal);
    
    LOG_INFO(LogModule::DBUS, "emitted network quality signal: quality='" << message << "', details='" << details << "', counter=" << counter);
    return ok;
}

bool DbusService::handlePing(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handlePing called");
    
    // 解析参数：目标主机名
    DBusError err;
    dbus_error_init(&err);
    const char* hostname = nullptr;
    
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &hostname, DBUS_TYPE_INVALID)) {
        LOG_ERROR(LogModule::DBUS, "Ping method error: " << err.message);
        dbus_error_free(&err);
        
        // 发送错误回复
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "Invalid arguments");
        std::lock_guard lock(output_mutex_);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return false;
    }
    
    if (!hostname || strlen(hostname) == 0) {
        LOG_ERROR(LogModule::DBUS, "Ping method error: empty hostname");
        
        // 发送错误回复
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "Empty hostname");
        std::lock_guard lock(output_mutex_);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return false;
    }
    
    LOG_INFO(LogModule::DBUS, "Ping request for host: " << hostname);
    
    // 获取当前上网网卡
    std::string currentIface;
    if (ctx_ && ctx_->weak_mgr) {
        auto ifaces = ctx_->weak_mgr->getCurrentInterfaces();
        for (const auto& net : ifaces) {
            if (net.usingNow()) {
                currentIface = net.ifName();
                break;
            }
        }
        if (currentIface.empty() && !ifaces.empty()) {
            currentIface = ifaces[0].ifName();
        }
    }
    
    if (currentIface.empty()) {
        LOG_ERROR(LogModule::DBUS, "Ping method error: no active interface found");
        
        // 发送错误回复
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "No active network interface");
        std::lock_guard lock(output_mutex_);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return false;
    }
    
    LOG_INFO(LogModule::DBUS, "Using interface: " << currentIface << " for ping to " << hostname);
    
    using MessagePtr = std::shared_ptr<DBusMessage>;
    MessagePtr request(dbus_message_ref(msg), [](DBusMessage* message) {
        dbus_message_unref(message);
    });
    const std::string host_copy(hostname);
    const bool accepted = ping_executor_->submit(
        host_copy, currentIface, 3000,
        [this, conn, request, host_copy, currentIface](int pingResult) {
            std::string result;
            if (pingResult >= 0) {
                result = "PING " + host_copy + " via " + currentIface + ": " +
                         std::to_string(pingResult) + "ms";
            } else {
                result = "PING " + host_copy + " via " + currentIface +
                         ": FAILED (error code: " + std::to_string(pingResult) + ")";
            }
            DBusMessage* reply = dbus_message_new_method_return(request.get());
            if (!reply) return;
            DBusMessageIter args;
            dbus_message_iter_init_append(reply, &args);
            const char* result_string = result.c_str();
            if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &result_string)) {
                dbus_message_unref(reply);
                return;
            }
            std::lock_guard lock(output_mutex_);
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
        });
    if (accepted) return true;

    DBusMessage* reply = dbus_message_new_error(
        msg, "com.example.WeakNet.Error.Busy", "Ping request queue is full");
    if (!reply) return false;
    {
        std::lock_guard lock(output_mutex_);
        dbus_connection_send(conn, reply, nullptr);
    }
    dbus_message_unref(reply);
    return false;
}

namespace {

using weaknet_dbus::v2::IncidentObservation;
using weaknet_dbus::v2::RootCauseEvidence;
using weaknet_dbus::v2::RootCauseHypothesisObservation;

bool appendVariantString(DBusMessageIter* dict, const char* key, const std::string& value) {
    DBusMessageIter entry, variant;
    const char* key_value = key;
    const char* string_value = value.c_str();
    return dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) &&
           dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key_value) &&
           dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant) &&
           dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &string_value) &&
           dbus_message_iter_close_container(&entry, &variant) &&
           dbus_message_iter_close_container(dict, &entry);
}

bool appendVariantStringArray(DBusMessageIter* dict, const char* key,
                              const std::vector<std::string>& values) {
    DBusMessageIter entry, variant, array;
    const char* key_value = key;
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) ||
        !dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key_value) ||
        !dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &variant) ||
        !dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &array)) return false;
    for (const auto& value : values) {
        const char* text = value.c_str();
        if (!dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &text)) return false;
    }
    return dbus_message_iter_close_container(&variant, &array) &&
           dbus_message_iter_close_container(&entry, &variant) &&
           dbus_message_iter_close_container(dict, &entry);
}

template <typename T>
bool appendVariantBasic(DBusMessageIter* dict, const char* key, int type,
                        const char* signature, T value) {
    DBusMessageIter entry, variant;
    const char* key_value = key;
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) ||
        !dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key_value) ||
        !dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, signature, &variant) ||
        !dbus_message_iter_append_basic(&variant, type, &value) ||
        !dbus_message_iter_close_container(&entry, &variant) ||
        !dbus_message_iter_close_container(dict, &entry)) return false;
    return true;
}

bool appendVariantBool(DBusMessageIter* dict, const char* key, bool value) {
    dbus_bool_t dbus_value = value ? 1 : 0;
    return appendVariantBasic(dict, key, DBUS_TYPE_BOOLEAN, "b", dbus_value);
}

bool appendDict(DBusMessageIter* array, const std::function<bool(DBusMessageIter*)>& fill) {
    DBusMessageIter dict;
    if (!dbus_message_iter_open_container(array, DBUS_TYPE_ARRAY, "{sv}", &dict)) return false;
    if (!fill(&dict)) return false;
    return dbus_message_iter_close_container(array, &dict);
}

std::string incidentTypeName(weaknet_dbus::v2::IncidentType type) {
    switch (type) {
        case weaknet_dbus::v2::IncidentType::HighTcpRtt: return "HighTcpRtt";
        case weaknet_dbus::v2::IncidentType::ElevatedTcpRetransmission: return "ElevatedTcpRetransmission";
        case weaknet_dbus::v2::IncidentType::RouteUnavailable: return "RouteUnavailable";
        case weaknet_dbus::v2::IncidentType::UplinkUnavailable: return "UplinkUnavailable";
        case weaknet_dbus::v2::IncidentType::SocketRouteConflict: return "SocketRouteConflict";
    }
    return "Unknown";
}

std::string scopeName(const weaknet_dbus::v2::IncidentScope& scope) {
    return std::visit([](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        std::ostringstream out;
        if constexpr (std::is_same_v<T, weaknet_dbus::v2::SocketId>) {
            out << "socket netns=" << value.netns.device << ":" << value.netns.inode
                << " generation=" << value.generation.value;
            if (value.cookie) out << " cookie=" << value.cookie->value;
        } else if constexpr (std::is_same_v<T, weaknet_dbus::v2::NetnsId>) {
            out << "netns=" << value.device << ":" << value.inode;
        } else {
            out << "interface netns-ifindex=" << value.ifindex;
        }
        return out.str();
    }, scope);
}

std::string rootCauseScopeName(const weaknet_dbus::v2::RootCauseScope& scope) {
    return std::visit([](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        std::ostringstream out;
        if constexpr (std::is_same_v<T, weaknet_dbus::v2::SocketId>) {
            out << "socket netns=" << value.netns.device << ":" << value.netns.inode
                << " generation=" << value.generation.value;
            if (value.cookie) out << " cookie=" << value.cookie->value;
        } else if constexpr (std::is_same_v<T, weaknet_dbus::v2::NetnsId>) {
            out << "netns=" << value.device << ":" << value.inode;
        } else {
            out << "interface ifindex=" << value.ifindex;
            if (!value.observed_name.empty()) out << " name=" << value.observed_name;
        }
        return out.str();
    }, scope);
}

std::string roleName(weaknet_dbus::v2::RootCauseEvidenceRole role) {
    switch (role) {
        case weaknet_dbus::v2::RootCauseEvidenceRole::Supporting: return "Supporting";
        case weaknet_dbus::v2::RootCauseEvidenceRole::Contradicting: return "Contradicting";
        case weaknet_dbus::v2::RootCauseEvidenceRole::Missing: return "Missing";
    }
    return "Unknown";
}

std::string evidenceKindName(weaknet_dbus::v2::RootCauseEvidenceKind kind) {
    return weaknet_dbus::v2::rootCauseEvidenceKindName(kind);
}

bool appendIncidentEvidenceArray(DBusMessageIter* dict, const char* key,
                                 const std::vector<weaknet_dbus::v2::IncidentEvidence>& evidence) {
    DBusMessageIter entry, variant, array;
    const char* key_value = key;
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) ||
        !dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key_value) ||
        !dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "aa{sv}", &variant) ||
        !dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "a{sv}", &array)) return false;
    for (const auto& item : evidence) {
        if (!appendDict(&array, [&](DBusMessageIter* item_dict) {
            bool ok = appendVariantString(item_dict, "source_kind", std::to_string(static_cast<unsigned>(item.source_kind)));
            ok = ok && appendVariantString(item_dict, "source", std::to_string(static_cast<unsigned>(item.source)));
            ok = ok && appendVariantString(item_dict, "condition", item.condition);
            ok = ok && appendVariantBasic(item_dict, "timestamp_ms", DBUS_TYPE_UINT64, "t",
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(item.observed_at.time_since_epoch()).count()));
            if (item.value) {
                if (std::holds_alternative<std::uint64_t>(item.value->value))
                    ok = ok && appendVariantBasic(item_dict, "value_uint64", DBUS_TYPE_UINT64, "t", std::get<std::uint64_t>(item.value->value));
                else
                    ok = ok && appendVariantBasic(item_dict, "value_double", DBUS_TYPE_DOUBLE, "d", std::get<double>(item.value->value));
            }
            return ok;
        })) return false;
    }
    return dbus_message_iter_close_container(&variant, &array) &&
           dbus_message_iter_close_container(&entry, &variant) &&
           dbus_message_iter_close_container(dict, &entry);
}

std::string rootCauseTypeName(weaknet_dbus::v2::RootCauseType type) {
    switch (type) {
        case weaknet_dbus::v2::RootCauseType::UplinkAvailabilityProblem: return "UplinkAvailabilityProblem";
        case weaknet_dbus::v2::RootCauseType::LocalRoutingProblem: return "LocalRoutingProblem";
        case weaknet_dbus::v2::RootCauseType::NetworkPathDegradation: return "NetworkPathDegradation";
        case weaknet_dbus::v2::RootCauseType::RemoteOrUpstreamDegradation: return "RemoteOrUpstreamDegradation";
        case weaknet_dbus::v2::RootCauseType::LocalLinkSuspected: return "LocalLinkSuspected";
        case weaknet_dbus::v2::RootCauseType::InsufficientEvidence: return "InsufficientEvidence";
    }
    return "Unknown";
}

std::string evidenceSummary(const std::vector<RootCauseEvidence>& evidence) {
    std::string result;
    for (const auto& item : evidence) {
        if (!result.empty()) result += "; ";
        result += evidenceKindName(item.kind);
        if (!item.provenance.empty()) result += " (" + item.provenance + ")";
    }
    return result.empty() ? "none" : result;
}

bool appendEvidenceArray(DBusMessageIter* dict, const char* key,
                         const std::vector<RootCauseEvidence>& evidence) {
    DBusMessageIter entry, variant, array;
    const char* key_value = key;
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) ||
        !dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key_value) ||
        !dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "aa{sv}", &variant) ||
        !dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "a{sv}", &array)) return false;
    for (const auto& item : evidence) {
        if (!appendDict(&array, [&](DBusMessageIter* item_dict) {
            bool ok = appendVariantString(item_dict, "role", roleName(item.role));
            ok = ok && appendVariantString(item_dict, "kind", evidenceKindName(item.kind));
            ok = ok && appendVariantString(item_dict, "scope", scopeName(item.scope));
            ok = ok && appendVariantString(item_dict, "source", std::to_string(static_cast<unsigned>(item.source)));
            ok = ok && appendVariantBasic(item_dict, "timestamp_ms", DBUS_TYPE_UINT64, "t",
                                          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(item.observed_at.time_since_epoch()).count()));
            ok = ok && appendVariantString(item_dict, "capability", std::to_string(static_cast<unsigned>(item.capability)));
            if (item.incident) ok = ok && appendVariantBasic(item_dict, "incident_id", DBUS_TYPE_UINT64, "t", item.incident->value);
            return ok;
        })) return false;
    }
    return dbus_message_iter_close_container(&variant, &array) &&
           dbus_message_iter_close_container(&entry, &variant) &&
           dbus_message_iter_close_container(dict, &entry);
}

bool appendIncidentArrayContents(DBusMessageIter* array, const std::vector<IncidentObservation>& incidents) {
    for (const auto& incident : incidents) {
        if (!appendDict(array, [&](DBusMessageIter* dict) {
            bool ok = appendVariantBasic(dict, "id", DBUS_TYPE_UINT64, "t", incident.id.value);
            ok = ok && appendVariantString(dict, "type", incidentTypeName(incident.type));
            ok = ok && appendVariantString(dict, "state", "Active");
            ok = ok && appendVariantString(dict, "scope", scopeName(incident.scope));
            ok = ok && appendVariantBasic(dict, "opened_at_ms", DBUS_TYPE_UINT64, "t", static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(incident.opened_at.time_since_epoch()).count()));
            ok = ok && appendVariantBasic(dict, "last_updated_at_ms", DBUS_TYPE_UINT64, "t", static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(incident.last_updated_at.time_since_epoch()).count()));
            ok = ok && appendIncidentEvidenceArray(dict, "evidence", incident.evidence);
            return ok;
        })) return false;
    }
    return true;
}

bool appendHypothesisArrayContents(DBusMessageIter* array, const std::vector<RootCauseHypothesisObservation>& hypotheses) {
    for (const auto& hypothesis : hypotheses) {
        if (!appendDict(array, [&](DBusMessageIter* dict) {
            bool ok = appendVariantBasic(dict, "occurrence", DBUS_TYPE_UINT64, "t", hypothesis.id.occurrence);
            ok = ok && appendVariantString(dict, "type", rootCauseTypeName(hypothesis.type));
            ok = ok && appendVariantString(dict, "state", "Active");
            ok = ok && appendVariantString(dict, "confidence", std::to_string(static_cast<unsigned>(hypothesis.confidence)));
            ok = ok && appendVariantString(dict, "scope", rootCauseScopeName(hypothesis.scope));
            ok = ok && appendVariantString(dict, "reason", hypothesis.reason_code);
            ok = ok && appendVariantString(dict, "supporting_summary", evidenceSummary(hypothesis.supporting_evidence));
            ok = ok && appendVariantString(dict, "contradicting_summary", evidenceSummary(hypothesis.contradicting_evidence));
            ok = ok && appendVariantString(dict, "missing_summary", evidenceSummary(hypothesis.missing_evidence));
            ok = ok && appendEvidenceArray(dict, "supporting_evidence", hypothesis.supporting_evidence);
            ok = ok && appendEvidenceArray(dict, "contradicting_evidence", hypothesis.contradicting_evidence);
            ok = ok && appendEvidenceArray(dict, "missing_evidence", hypothesis.missing_evidence);
            return ok;
        })) return false;
    }
    return true;
}

bool appendVariantArrayField(DBusMessageIter* dict, const char* key,
                             const std::function<bool(DBusMessageIter*)>& fill) {
    DBusMessageIter entry, variant, array;
    const char* key_value = key;
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) ||
        !dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key_value) ||
        !dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "aa{sv}", &variant) ||
        !dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "a{sv}", &array) ||
        !fill(&array)) return false;
    return dbus_message_iter_close_container(&variant, &array) &&
           dbus_message_iter_close_container(&entry, &variant) &&
           dbus_message_iter_close_container(dict, &entry);
}

bool sendV2Reply(DBusConnection* conn, DBusMessage* msg,
                 const std::function<bool(DBusMessageIter*)>& fill) {
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;
    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const bool ok = fill(&args) && dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
    return ok;
}

bool sendV2DictReply(DBusConnection* conn, DBusMessage* msg,
                     const std::function<bool(DBusMessageIter*)>& fill) {
    return sendV2Reply(conn, msg, [&](DBusMessageIter* args) {
        DBusMessageIter dict;
        if (!dbus_message_iter_open_container(args, DBUS_TYPE_ARRAY, "{sv}", &dict)) return false;
        const bool ok = fill(&dict);
        return ok && dbus_message_iter_close_container(args, &dict);
    });
}

bool sendV2Error(DBusConnection* conn, DBusMessage* msg, const char* name, const char* text) {
    DBusMessage* reply = dbus_message_new_error(msg, name, text);
    if (!reply) return false;
    const bool ok = dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
    return ok;
}

}  // namespace

bool DbusService::handleV2GetStatus(DBusConnection* conn, DBusMessage* msg) {
    if (!ctx_ || !ctx_->diagnostics) return sendV2Error(conn, msg, "com.example.WeakNet.Error.Unavailable", "diagnostics unavailable");
    const auto status = ctx_->diagnostics->getStatusSnapshot();
    return sendV2DictReply(conn, msg, [&](DBusMessageIter* dict) {
        bool ok = appendVariantString(dict, "state", v2::overallDiagnosticStateName(status.state));
        ok = ok && appendVariantBool(dict, "topology_authoritative", status.topology_authoritative);
        ok = ok && appendVariantBool(dict, "topology_degraded", status.topology_degraded);
        ok = ok && appendVariantBool(dict, "socket_tracker_degraded", status.socket_tracker_degraded);
        ok = ok && appendVariantBool(dict, "runtime_degraded", status.runtime_degraded);
        ok = ok && appendVariantBasic(dict, "active_incidents", DBUS_TYPE_UINT64, "t", static_cast<std::uint64_t>(status.active_incidents));
        ok = ok && appendVariantBasic(dict, "active_hypotheses", DBUS_TYPE_UINT64, "t", static_cast<std::uint64_t>(status.active_hypotheses));
        ok = ok && appendVariantString(dict, "uplink", status.uplink && status.uplink->interface ? status.uplink->interface->observed_name : "unavailable");
        if (status.uplink && status.uplink->interface)
            ok = ok && appendVariantBasic(dict, "uplink_ifindex", DBUS_TYPE_UINT32, "u", status.uplink->interface->ifindex);
        ok = ok && appendVariantBasic(dict, "timestamp_ms", DBUS_TYPE_UINT64, "t", status.timestamp_ms);
        return ok;
    });
}

bool DbusService::handleV2ListIncidents(DBusConnection* conn, DBusMessage* msg) {
    if (!ctx_ || !ctx_->diagnostics) return sendV2Error(conn, msg, "com.example.WeakNet.Error.Unavailable", "diagnostics unavailable");
    return sendV2Reply(conn, msg, [&](DBusMessageIter* args) {
        DBusMessageIter array;
        if (!dbus_message_iter_open_container(args, DBUS_TYPE_ARRAY, "a{sv}", &array)) return false;
        const bool ok = appendIncidentArrayContents(&array, ctx_->diagnostics->listActiveIncidents());
        return ok && dbus_message_iter_close_container(args, &array);
    });
}

bool DbusService::handleV2ListHypotheses(DBusConnection* conn, DBusMessage* msg) {
    if (!ctx_ || !ctx_->diagnostics) return sendV2Error(conn, msg, "com.example.WeakNet.Error.Unavailable", "diagnostics unavailable");
    return sendV2Reply(conn, msg, [&](DBusMessageIter* args) {
        DBusMessageIter array;
        if (!dbus_message_iter_open_container(args, DBUS_TYPE_ARRAY, "a{sv}", &array)) return false;
        const bool ok = appendHypothesisArrayContents(&array, ctx_->diagnostics->listRootCauses());
        return ok && dbus_message_iter_close_container(args, &array);
    });
}

bool DbusService::handleV2GetDiagnosis(DBusConnection* conn, DBusMessage* msg) {
    if (!ctx_ || !ctx_->diagnostics) return sendV2Error(conn, msg, "com.example.WeakNet.Error.Unavailable", "diagnostics unavailable");
    const auto diagnosis = ctx_->diagnostics->getDiagnosisSnapshot();
    return sendV2DictReply(conn, msg, [&](DBusMessageIter* dict) {
        bool ok = appendVariantString(dict, "state", v2::overallDiagnosticStateName(diagnosis.status.state));
        ok = ok && appendVariantBasic(dict, "active_incidents", DBUS_TYPE_UINT64, "t", static_cast<std::uint64_t>(diagnosis.incidents.size()));
        ok = ok && appendVariantBasic(dict, "active_hypotheses", DBUS_TYPE_UINT64, "t", static_cast<std::uint64_t>(diagnosis.hypotheses.size()));
        ok = ok && appendVariantArrayField(dict, "incidents", [&](DBusMessageIter* array) {
            return appendIncidentArrayContents(array, diagnosis.incidents);
        });
        ok = ok && appendVariantArrayField(dict, "hypotheses", [&](DBusMessageIter* array) {
            return appendHypothesisArrayContents(array, diagnosis.hypotheses);
        });
        ok = ok && appendVariantString(dict, "selected_uplink",
            diagnosis.status.uplink && diagnosis.status.uplink->interface
                ? diagnosis.status.uplink->interface->observed_name : "unavailable");
        ok = ok && appendVariantStringArray(dict, "limitations", diagnosis.limitations);
        return ok;
    });
}

bool DbusService::handleV2GetTopology(DBusConnection* conn, DBusMessage* msg) {
    if (!ctx_ || !ctx_->diagnostics) return sendV2Error(conn, msg, "com.example.WeakNet.Error.Unavailable", "diagnostics unavailable");
    const auto topology = ctx_->diagnostics->getTopologySnapshot();
    const auto uplink = ctx_->diagnostics->getStatusSnapshot().uplink;
    return sendV2DictReply(conn, msg, [&](DBusMessageIter* dict) {
        bool ok = appendVariantBool(dict, "authoritative", topology.authoritative);
        ok = ok && appendVariantBool(dict, "partial", topology.partial);
        ok = ok && appendVariantBool(dict, "degraded", topology.degraded);
        ok = ok && appendVariantBasic(dict, "generation", DBUS_TYPE_UINT64, "t", topology.generation);
        ok = ok && appendVariantBasic(dict, "link_count", DBUS_TYPE_UINT64, "t", static_cast<std::uint64_t>(topology.links.size()));
        ok = ok && appendVariantBasic(dict, "route_count", DBUS_TYPE_UINT64, "t", static_cast<std::uint64_t>(topology.routes.size()));
        ok = ok && appendVariantString(dict, "selected_uplink", uplink && uplink->interface ? uplink->interface->observed_name : "unavailable");
        if (uplink && uplink->interface)
            ok = ok && appendVariantBasic(dict, "selected_uplink_ifindex", DBUS_TYPE_UINT32, "u", uplink->interface->ifindex);
        return ok;
    });
}

}  // namespace weaknet_dbus
