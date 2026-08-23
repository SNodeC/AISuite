/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex2/protocol/Envelope.h"

namespace ai::openai::codex2::protocol {

    std::string_view toString(Role role) noexcept {
        return role == Role::Controller ? "controller" : "observer";
    }

    JsonRpcKind classifyJsonRpc(const nlohmann::json& message) noexcept {
        if (!message.is_object()) {
            return JsonRpcKind::Invalid;
        }
        const bool hasMethod = message.contains("method") && message["method"].is_string();
        const bool hasId = message.contains("id") && !message["id"].is_null();
        const bool hasResult = message.contains("result");
        const bool hasError = message.contains("error");
        if (hasMethod) {
            return hasId ? JsonRpcKind::Request : JsonRpcKind::Notification;
        }
        if (hasId && (hasResult != hasError)) {
            return JsonRpcKind::Response;
        }
        return JsonRpcKind::Invalid;
    }

    std::optional<std::string> jsonRpcIdKey(const nlohmann::json& message) {
        if (!message.is_object() || !message.contains("id") || message["id"].is_null()) {
            return std::nullopt;
        }
        const nlohmann::json& id = message["id"];
        if (!id.is_string() && !id.is_number_integer() && !id.is_number_unsigned()) {
            return std::nullopt;
        }
        return id.dump();
    }

    std::optional<std::string> jsonRpcMethod(const nlohmann::json& message) {
        if (!message.is_object()) {
            return std::nullopt;
        }
        const auto method = message.find("method");
        if (method == message.end() || !method->is_string()) {
            return std::nullopt;
        }
        return method->get<std::string>();
    }

    nlohmann::json appServerEnvelope(std::string_view connectionId,
                                     Role role,
                                     std::uint64_t sequence,
                                     const nlohmann::json& payload) {
        return {{"kind", "appserver"},
                {"connectionId", connectionId},
                {"role", toString(role)},
                {"seq", sequence},
                {"payload", payload}};
    }

    nlohmann::json connectionEvent(std::string_view event,
                                   std::string_view connectionId,
                                   Role role,
                                   std::uint64_t sequence) {
        return {{"kind", "bridge.connection"},
                {"event", event},
                {"connectionId", connectionId},
                {"role", toString(role)},
                {"seq", sequence}};
    }

    nlohmann::json controllerEvent(std::optional<std::string_view> controllerConnectionId, std::uint64_t sequence) {
        nlohmann::json event{{"kind", "bridge.controller"}, {"seq", sequence}};
        event["controllerConnectionId"] = controllerConnectionId ? nlohmann::json(*controllerConnectionId) : nlohmann::json(nullptr);
        return event;
    }

    nlohmann::json diagnosticEvent(std::string_view code,
                                   std::string_view message,
                                   std::optional<std::string_view> connectionId,
                                   std::uint64_t sequence,
                                   nlohmann::json details) {
        nlohmann::json event{{"kind", "bridge.diagnostic"},
                             {"code", code},
                             {"message", message},
                             {"seq", sequence},
                             {"details", std::move(details)}};
        event["connectionId"] = connectionId ? nlohmann::json(*connectionId) : nlohmann::json(nullptr);
        return event;
    }

    nlohmann::json jsonRpcError(const nlohmann::json& id, int code, std::string_view message, nlohmann::json data) {
        nlohmann::json error{{"code", code}, {"message", message}};
        if (!data.is_null()) {
            error["data"] = std::move(data);
        }
        return {{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(error)}};
    }

} // namespace ai::openai::codex2::protocol
