/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex2/frontend/CodexBridge.h"

#include <utility>

namespace ai::openai::codex2::frontend {

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
                bridgeEventHandler_(bridgeMessage);
            }
            return true;
        }

        const auto payload = bridgeMessage.find("payload");
        if (payload == bridgeMessage.end() || !payload->is_object()) {
            return false;
        }
        if (rawHandler_) {
            rawHandler_(protocol::AppServerDirection::FromAppServer, *payload);
        }

        const protocol::JsonRpcKind messageKind = protocol::classifyJsonRpc(*payload);
        const std::optional<std::string> id = protocol::jsonRpcIdKey(*payload);
        if (messageKind == protocol::JsonRpcKind::Response && id) {
            const auto pending = pending_.find(*id);
            if (pending != pending_.end()) {
                PendingHandler handler = std::move(pending->second);
                pending_.erase(pending);
                if (handler) {
                    handler(*payload);
                }
            }
            return true;
        }

        const std::optional<std::string> method = protocol::jsonRpcMethod(*payload);
        if (messageKind == protocol::JsonRpcKind::Request && method) {
            const auto handler = serverRequestHandlers_.find(*method);
            if (handler != serverRequestHandlers_.end() && handler->second) {
                EventDispatcher dispatcher = handler->second;
                dispatcher(*payload);
            }
            return true;
        }
        if (messageKind == protocol::JsonRpcKind::Notification && method) {
            const auto handler = serverNotificationHandlers_.find(*method);
            if (handler != serverNotificationHandlers_.end() && handler->second) {
                EventDispatcher dispatcher = handler->second;
                dispatcher(*payload);
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
        auto pending = std::move(pending_);
        pending_.clear();
        connectionId_.reset();
        controllerConnectionId_.reset();
        role_.reset();
        for (auto& [key, handler] : pending) {
            if (!handler) {
                continue;
            }
            try {
                handler(protocol::jsonRpcError(nlohmann::json::parse(key), -32020, reason));
            } catch (const nlohmann::json::exception&) {
                handler(protocol::jsonRpcError(nullptr, -32020, reason));
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

    std::string CodexBridge::requestTyped(std::string_view method,
                                          std::optional<nlohmann::json> params,
                                          PendingHandler handler) {
        const std::string id = nextRequestId();
        const std::string key = nlohmann::json(id).dump();
        nlohmann::json request{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
        if (params) {
            request["params"] = std::move(*params);
        }

        if (!connectionId_) {
            if (handler) {
                handler(protocol::jsonRpcError(id, -32021, "frontend bridge connection is not established"));
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
                    failed(protocol::jsonRpcError(id, -32020, "frontend bridge transport rejected request"));
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
            rawHandler_(protocol::AppServerDirection::ToAppServer, message);
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
        const std::string kind = message.value("kind", std::string{});
        if (kind == "bridge.connection" && message.value("event", std::string{}) == "opened") {
            const auto id = message.find("connectionId");
            if (id != message.end() && id->is_string()) {
                connectionId_ = id->get<std::string>();
            }
            const std::string role = message.value("role", std::string{});
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
        }
    }

    std::string CodexBridge::nextRequestId() {
        const std::string prefix = connectionId_.value_or("unconnected-frontend");
        return prefix + "-request-" + std::to_string(nextRequestNumber_++);
    }

} // namespace ai::openai::codex2::frontend
