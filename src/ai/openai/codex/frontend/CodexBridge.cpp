/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/CodexBridge.h"

#include <iostream>
#include <utility>

namespace ai::openai::codex::frontend {

    CodexBridge::CodexBridge(Sender sender)
        : sender_(std::move(sender)) {
    }

    void CodexBridge::setSender(Sender sender) {
        sender_ = std::move(sender);
    }

    bool CodexBridge::receive(const nlohmann::json& bridgeMessage) {
        if (!bridgeMessage.is_object()) {
            return false;
        }
        const auto kind = bridgeMessage.find("kind");
        if (kind == bridgeMessage.end() || !kind->is_string()) {
            return false;
        }
        if (*kind != "appserver") {
            updateBridgeState(bridgeMessage);
            if (bridgeEventHandler_) {
                try {
                    bridgeEventHandler_(bridgeMessage);
                } catch (...) {
                    std::clog << "codex-bridge-client: bridge event callback failed\n";
                }
            }
            return true;
        }

        const auto payload = bridgeMessage.find("payload");
        if (payload == bridgeMessage.end() || !payload->is_object()) {
            return false;
        }
        if (rawHandler_) {
            try {
                rawHandler_(protocol::AppServerDirection::FromAppServer, *payload);
            } catch (...) {
                std::clog << "codex-bridge-client: raw inbound callback failed\n";
            }
        }

        const protocol::JsonRpcKind messageKind = protocol::classifyJsonRpc(*payload);
        const std::optional<std::string> id = protocol::jsonRpcIdKey(*payload);
        if (messageKind == protocol::JsonRpcKind::Response && id) {
            const auto pending = pending_.find(*id);
            if (pending != pending_.end()) {
                PendingHandler handler = std::move(pending->second);
                pending_.erase(pending);
                if (handler) {
                    try {
                        handler(*payload);
                    } catch (...) {
                        std::clog << "codex-bridge-client: response callback failed\n";
                    }
                }
            }
            return true;
        }

        const std::optional<std::string> method = protocol::jsonRpcMethod(*payload);
        if (messageKind == protocol::JsonRpcKind::Request && method) {
            const auto handler = serverRequestHandlers_.find(*method);
            if (handler != serverRequestHandlers_.end() && handler->second) {
                EventDispatcher dispatcher = handler->second;
                try {
                    dispatcher(*payload);
                } catch (...) {
                    std::clog << "codex-bridge-client: server-request callback failed\n";
                    if (id) {
                        static_cast<void>(sendServerError((*payload)["id"],
                                                          -32603,
                                                          "frontend request handler failed",
                                                          nullptr));
                    }
                }
            } else if (id) {
                static_cast<void>(sendServerError((*payload)["id"],
                                                  -32601,
                                                  "frontend has no handler for the server request",
                                                  nullptr));
            }
            return true;
        }
        if (messageKind == protocol::JsonRpcKind::Notification && method) {
            const auto handler = serverNotificationHandlers_.find(*method);
            if (handler != serverNotificationHandlers_.end() && handler->second) {
                EventDispatcher dispatcher = handler->second;
                try {
                    dispatcher(*payload);
                } catch (...) {
                    std::clog << "codex-bridge-client: notification callback failed\n";
                }
            }
            return true;
        }
        return messageKind != protocol::JsonRpcKind::Invalid;
    }

    bool CodexBridge::sendRawJson(const nlohmann::json& appServerMessage) {
        return protocol::classifyJsonRpc(appServerMessage) != protocol::JsonRpcKind::Invalid &&
            sendAppServerMessage(appServerMessage);
    }

    void CodexBridge::onRawJson(RawHandler handler) {
        rawHandler_ = std::move(handler);
    }

    void CodexBridge::onBridgeEvent(BridgeEventHandler handler) {
        bridgeEventHandler_ = std::move(handler);
    }

    void CodexBridge::transportDisconnected(std::string_view reason) {
        failPending(reason, -32020);
        connectionId_.reset();
        controllerConnectionId_.reset();
        role_.reset();
        providerGeneration_ = 0;
        providerReady_ = false;
    }

    void CodexBridge::failPending(std::string_view reason, int code) noexcept {
        auto pending = std::move(pending_);
        pending_.clear();
        for (auto& [key, handler] : pending) {
            if (!handler) {
                continue;
            }
            nlohmann::json id = nullptr;
            try {
                id = nlohmann::json::parse(key);
            } catch (const nlohmann::json::exception&) {
            }
            try {
                handler(protocol::jsonRpcError(id, code, reason));
            } catch (...) {
                // A lifecycle callback is invoked exactly once and may never
                // escape into a noexcept transport teardown path.
            }
        }
    }

    bool CodexBridge::claimController() {
        return sendBridgeCommand({{"kind", "bridge.controller"}, {"action", "claim"}});
    }

    bool CodexBridge::releaseController() {
        return sendBridgeCommand({{"kind", "bridge.controller"}, {"action", "release"}});
    }

    bool CodexBridge::transferController(std::string targetConnectionId) {
        return sendBridgeCommand({{"kind", "bridge.controller"},
                                  {"action", "transfer"},
                                  {"targetConnectionId", std::move(targetConnectionId)}});
    }

    const std::optional<std::string>& CodexBridge::connectionId() const noexcept {
        return connectionId_;
    }

    const std::optional<std::string>& CodexBridge::controllerConnectionId() const noexcept {
        return controllerConnectionId_;
    }

    std::optional<protocol::Role> CodexBridge::role() const noexcept {
        return role_;
    }

    bool CodexBridge::isController() const noexcept {
        return role_ == protocol::Role::Controller;
    }

    std::uint64_t CodexBridge::providerGeneration() const noexcept {
        return providerGeneration_;
    }

    bool CodexBridge::providerReady() const noexcept {
        return providerReady_;
    }

    std::string CodexBridge::requestTyped(std::string_view method,
                                          std::optional<nlohmann::json> params,
                                          PendingHandler handler) {
        const std::string id = nextRequestId();
        const std::string key = nlohmann::json(id).dump();
        nlohmann::json request{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
        if (params) {
            request["params"] = std::move(*params);
        }

        if (!connectionId_ || !providerReady_) {
            if (handler) {
                try {
                    handler(protocol::jsonRpcError(
                        id,
                        connectionId_ ? -32002 : -32021,
                        connectionId_ ? "app-server provider is not ready"
                                      : "frontend bridge connection is not established"));
                } catch (...) {
                    std::clog << "codex-bridge-client: rejected request callback failed\n";
                }
            }
            return id;
        }
        pending_[key] = std::move(handler);
        if (!sendAppServerMessage(request)) {
            const auto pending = pending_.find(key);
            if (pending != pending_.end()) {
                PendingHandler failed = std::move(pending->second);
                pending_.erase(pending);
                if (failed) {
                    try {
                        failed(protocol::jsonRpcError(
                            id, -32020,
                            "frontend bridge transport rejected request"));
                    } catch (...) {
                        std::clog << "codex-bridge-client: send-failure callback failed\n";
                    }
                }
            }
        }
        return id;
    }

    bool CodexBridge::sendNotification(std::string_view method, std::optional<nlohmann::json> params) {
        nlohmann::json notification{{"jsonrpc", "2.0"}, {"method", method}};
        if (params) {
            notification["params"] = std::move(*params);
        }
        return connectionId_ && sendAppServerMessage(notification);
    }

    bool CodexBridge::sendServerResponse(const nlohmann::json& id, const nlohmann::json& result) {
        return id.is_null() ? false : sendAppServerMessage({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
    }

    bool CodexBridge::sendServerError(const nlohmann::json& id,
                                      int code,
                                      std::string_view message,
                                      nlohmann::json data) {
        return id.is_null() ? false : sendAppServerMessage(protocol::jsonRpcError(id, code, message, std::move(data)));
    }

    bool CodexBridge::sendAppServerMessage(const nlohmann::json& message) {
        if (!connectionId_ || !sender_ || !sender_({{"kind", "appserver"}, {"payload", message}})) {
            return false;
        }
        if (rawHandler_) {
            try {
                rawHandler_(protocol::AppServerDirection::ToAppServer, message);
            } catch (...) {
                std::clog << "codex-bridge-client: raw outbound callback failed\n";
            }
        }
        return true;
    }

    bool CodexBridge::sendBridgeCommand(nlohmann::json command) {
        return connectionId_ && sender_ && sender_(command);
    }

    void CodexBridge::registerServerRequestHandler(std::string_view method, EventDispatcher handler) {
        if (handler) {
            serverRequestHandlers_[std::string(method)] = std::move(handler);
        } else {
            serverRequestHandlers_.erase(std::string(method));
        }
    }

    void CodexBridge::registerServerNotificationHandler(std::string_view method, EventDispatcher handler) {
        if (handler) {
            serverNotificationHandlers_[std::string(method)] = std::move(handler);
        } else {
            serverNotificationHandlers_.erase(std::string(method));
        }
    }

    void CodexBridge::updateBridgeState(const nlohmann::json& message) {
        const auto kindMember = message.find("kind");
        if (kindMember == message.end() || !kindMember->is_string()) {
            return;
        }
        const std::string kind = kindMember->get<std::string>();
        const auto stringMember = [&message](std::string_view name) {
            const auto member = message.find(name);
            return member != message.end() && member->is_string() ? member->get<std::string>() : std::string{};
        };
        if (kind == "bridge.connection" && stringMember("event") == "opened") {
            const auto id = message.find("connectionId");
            if (id != message.end() && id->is_string()) {
                connectionId_ = id->get<std::string>();
            }
            const std::string role = stringMember("role");
            role_ = role == "controller" ? std::optional<protocol::Role>(protocol::Role::Controller)
                                         : role == "observer" ? std::optional<protocol::Role>(protocol::Role::Observer)
                                                              : std::nullopt;
        } else if (kind == "bridge.controller") {
            const auto controller = message.find("controllerConnectionId");
            if (controller != message.end() && controller->is_string()) {
                controllerConnectionId_ = controller->get<std::string>();
            } else {
                controllerConnectionId_.reset();
            }
            if (connectionId_) {
                role_ = controllerConnectionId_ && *controllerConnectionId_ == *connectionId_ ? protocol::Role::Controller
                                                                                              : protocol::Role::Observer;
            }
        } else if (kind == "bridge.provider") {
            const auto generation = message.find("providerGeneration");
            if (generation == message.end() ||
                (!generation->is_number_unsigned() && !generation->is_number_integer())) {
                return;
            }
            std::uint64_t nextGeneration = 0;
            if (generation->is_number_unsigned()) {
                nextGeneration = generation->get<std::uint64_t>();
            } else {
                const std::int64_t generationValue = generation->get<std::int64_t>();
                if (generationValue < 0) {
                    return;
                }
                nextGeneration = static_cast<std::uint64_t>(generationValue);
            }
            const std::string state = stringMember("state");
            if (nextGeneration < providerGeneration_) {
                return;
            }
            if (providerGeneration_ != 0 && nextGeneration > providerGeneration_) {
                failPending("app-server provider generation changed", -32002);
            }
            providerGeneration_ = nextGeneration;
            providerReady_ = state == "ready";
            if (state == "disconnected") {
                const std::string reason = stringMember("reason");
                failPending(reason.empty() ? std::string_view{"app-server disconnected"}
                                           : std::string_view{reason},
                            -32002);
            }
        }
    }

    std::string CodexBridge::nextRequestId() {
        const std::string prefix = connectionId_.value_or("unconnected-frontend");
        return prefix + "-request-" + std::to_string(nextRequestNumber_++);
    }

} // namespace ai::openai::codex::frontend
