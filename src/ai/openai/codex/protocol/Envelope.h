/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_PROTOCOL_ENVELOPE_H
#define AI_OPENAI_CODEX_PROTOCOL_ENVELOPE_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace ai::openai::codex::protocol {

    enum class Role { Controller, Observer };
    enum class AppServerDirection { ToAppServer, FromAppServer };
    enum class JsonRpcKind { Request, Notification, Response, Invalid };

    std::string_view toString(Role role) noexcept;
    JsonRpcKind classifyJsonRpc(const nlohmann::json& message) noexcept;
    std::optional<std::string> jsonRpcIdKey(const nlohmann::json& message);
    std::optional<std::string> jsonRpcMethod(const nlohmann::json& message);

    nlohmann::json appServerEnvelope(std::string_view connectionId,
                                     Role role,
                                     std::uint64_t sequence,
                                     const nlohmann::json& payload);
    nlohmann::json connectionEvent(std::string_view event,
                                   std::string_view connectionId,
                                   Role role,
                                   std::uint64_t sequence);
    nlohmann::json controllerEvent(std::optional<std::string_view> controllerConnectionId, std::uint64_t sequence);
    nlohmann::json diagnosticEvent(std::string_view code,
                                   std::string_view message,
                                   std::optional<std::string_view> connectionId,
                                   std::uint64_t sequence,
                                   nlohmann::json details = nlohmann::json::object());
    nlohmann::json jsonRpcError(const nlohmann::json& id, int code, std::string_view message, nlohmann::json data = nullptr);

} // namespace ai::openai::codex::protocol

#endif
