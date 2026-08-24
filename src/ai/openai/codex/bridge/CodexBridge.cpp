/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/bridge/CodexBridge.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ai::openai::codex::bridge {

    CodexBridge::CodexBridge(CodexBridgeOptions options)
        : options_(options) {
    }

    void CodexBridge::setAppServer(AppServerEndpoint* appServer) {
        if (appServer != nullptr && appServer_ != nullptr && appServer_ != appServer) {
            throw std::logic_error("CodexBridge already has an app-server endpoint");
        }
        appServer_ = appServer;
    }

    std::string CodexBridge::registerFrontend(FrontendEndpoint& frontend) {
        const std::string id = "frontend-" + std::to_string(nextConnectionNumber_++);
        const bool becomesController = options_.firstFrontendBecomesController && !controller_.has_value();
        frontends_.emplace(id, FrontendRecord{&frontend, becomesController ? protocol::Role::Controller : protocol::Role::Observer});
        if (becomesController) {
            controller_ = id;
        }
        sendTo(id, protocol::connectionEvent("opened", id, frontends_.at(id).role, nextSequence()));
        broadcastController();
        return id;
    }

    void CodexBridge::unregisterFrontend(std::string_view connectionId) {
        const auto frontend = frontends_.find(std::string(connectionId));
        if (frontend == frontends_.end()) {
            return;
        }

        failFrontendServerRequests(connectionId, "frontend disconnected before responding");
        std::erase_if(frontendRequestOwners_, [connectionId](const auto& entry) {
            return entry.second.connectionId == connectionId;
        });

        const bool wasController = controller_ && *controller_ == connectionId;
        frontends_.erase(frontend);
        if (wasController) {
            controller_.reset();
            broadcastController();
        }
    }

    void CodexBridge::receiveFromFrontend(std::string_view connectionId, const nlohmann::json& message) {
        if (!frontends_.contains(std::string(connectionId))) {
            return;
        }
        if (!message.is_object() || !message.contains("kind") || !message["kind"].is_string()) {
            reject(connectionId, nlohmann::json::object(), -32600, "bridge message must be an object with a string kind");
            return;
        }

        const std::string kind = message["kind"].get<std::string>();
        if (kind == "appserver") {
            handleAppServerEnvelope(connectionId, message);
        } else if (kind == "bridge.controller") {
            handleControllerCommand(connectionId, message);
        } else {
            reject(connectionId, nlohmann::json::object(), -32600, "unsupported bridge message kind");
        }
    }

    void CodexBridge::receiveFromAppServer(const nlohmann::json& message) {
        if (rawHandler_) {
            rawHandler_(protocol::AppServerDirection::FromAppServer, message);
        }

        const protocol::JsonRpcKind kind = protocol::classifyJsonRpc(message);
        const std::optional<std::string> id = protocol::jsonRpcIdKey(message);
        if (kind == protocol::JsonRpcKind::Response && id) {
            const auto local = localRequests_.find(*id);
            if (local != localRequests_.end()) {
                LocalResponseHandler handler = std::move(local->second.handler);
                localRequests_.erase(local);
                if (handler) {
                    handler(message);
                }
                return;
            }

            const auto owner = frontendRequestOwners_.find(*id);
            if (owner != frontendRequestOwners_.end()) {
                const FrontendRequest request = owner->second;
                frontendRequestOwners_.erase(owner);
                const auto frontend = frontends_.find(request.connectionId);
                if (frontend != frontends_.end()) {
                    nlohmann::json response = message;
                    response["id"] = request.frontendId;
                    sendTo(request.connectionId,
                           protocol::appServerEnvelope(request.connectionId, frontend->second.role, nextSequence(), response));
                }
                return;
            }

            emitDiagnostic("unmatched-appserver-response",
                           "app-server response has no local or frontend request owner",
                           std::nullopt,
                           {{"id", message["id"]}});
            return;
        }

        if (kind == protocol::JsonRpcKind::Request && id) {
            const std::optional<std::string> method = protocol::jsonRpcMethod(message);
            if (!method) {
                emitDiagnostic("invalid-appserver-request", "app-server request has no method", std::nullopt);
                return;
            }
            if (appServerRequestOwners_.contains(*id)) {
                sendToAppServer(protocol::jsonRpcError(message["id"], -32600, "duplicate app-server request id"));
                emitDiagnostic("duplicate-appserver-request-id",
                               "app-server reused an outstanding server-request id",
                               std::nullopt,
                               {{"id", message["id"]}, {"method", *method}});
                return;
            }

            const auto localHandler = serverRequestHandlers_.find(*method);
            if (localHandler != serverRequestHandlers_.end() && localHandler->second) {
                appServerRequestOwners_.emplace(*id,
                                                ServerRequestOwner{ServerRequestOwnerKind::LocalApplication, std::string{}, message["id"]});
                LocalEventHandler handler = localHandler->second;
                handler(message);
                return;
            }

            if (controller_) {
                const auto controller = frontends_.find(*controller_);
                if (controller != frontends_.end()) {
                    appServerRequestOwners_.emplace(*id, ServerRequestOwner{ServerRequestOwnerKind::Frontend, *controller_, message["id"]});
                    sendTo(*controller_, protocol::appServerEnvelope(*controller_, controller->second.role, nextSequence(), message));
                    return;
                }
            }

            sendToAppServer(protocol::jsonRpcError(message["id"], -32010, "no handler or controller is available"));
            emitDiagnostic("appserver-request-without-handler",
                           "app-server request cannot be delivered because no local handler or controller is available",
                           std::nullopt,
                           {{"id", message["id"]}, {"method", *method}});
            return;
        }

        if (kind == protocol::JsonRpcKind::Notification) {
            const std::optional<std::string> method = protocol::jsonRpcMethod(message);
            if (method) {
                if (*method == "serverRequest/resolved") {
                    const auto params = message.find("params");
                    if (params != message.end() && params->is_object()) {
                        const auto requestId = params->find("requestId");
                        if (requestId != params->end() &&
                            (requestId->is_string() || requestId->is_number_integer() || requestId->is_number_unsigned())) {
                            appServerRequestOwners_.erase(requestId->dump());
                        }
                    }
                }
                const auto localHandler = serverNotificationHandlers_.find(*method);
                if (localHandler != serverNotificationHandlers_.end() && localHandler->second) {
                    LocalEventHandler handler = localHandler->second;
                    handler(message);
                }
            }
            std::vector<std::string> ids;
            ids.reserve(frontends_.size());
            for (const auto& [connectionId, record] : frontends_) {
                static_cast<void>(record);
                ids.push_back(connectionId);
            }
            for (const std::string& connectionId : ids) {
                const auto frontend = frontends_.find(connectionId);
                if (frontend != frontends_.end()) {
                    sendTo(connectionId, protocol::appServerEnvelope(connectionId, frontend->second.role, nextSequence(), message));
                }
            }
            return;
        }

        emitDiagnostic("invalid-appserver-message", "app-server emitted an invalid JSON-RPC message", std::nullopt);
    }

    void CodexBridge::appServerConnected() {
        appServerReady_ = false;
        ++providerGeneration_;
        emitDiagnostic(
            "appserver-connected", "app-server transport connected", std::nullopt, {{"providerGeneration", providerGeneration_}});
        if (providerLifecycleHandler_) {
            providerLifecycleHandler_(true);
        }
    }

    void CodexBridge::appServerDisconnected(std::string_view reason) {
        appServerReady_ = false;
        failLocalRequests(reason);
        failFrontendRequests(reason);
        appServerRequestOwners_.clear();
        emitDiagnostic("appserver-disconnected",
                       "app-server transport disconnected",
                       std::nullopt,
                       {{"reason", reason}, {"providerGeneration", providerGeneration_}});
        if (providerLifecycleHandler_) {
            providerLifecycleHandler_(false);
        }
    }

    void CodexBridge::setAppServerReady() {
        if (appServer_ == nullptr || !appServer_->isConnected() || appServerReady_) {
            return;
        }
        appServerReady_ = true;
        emitDiagnostic("appserver-ready", "app-server session initialized", std::nullopt, {{"providerGeneration", providerGeneration_}});
    }

    void CodexBridge::onProviderLifecycle(ProviderLifecycleHandler handler) {
        providerLifecycleHandler_ = std::move(handler);
    }

    bool CodexBridge::sendRawJson(const nlohmann::json& message) {
        const protocol::JsonRpcKind kind = protocol::classifyJsonRpc(message);
        const std::optional<std::string> id = protocol::jsonRpcIdKey(message);
        if (kind == protocol::JsonRpcKind::Invalid) {
            return false;
        }

        if (kind == protocol::JsonRpcKind::Request) {
            if (!id || localRequests_.contains(*id) || frontendRequestOwners_.contains(*id)) {
                return false;
            }
            localRequests_.emplace(*id, LocalRequest{message["id"], LocalResponseHandler{}});
            if (!sendToAppServer(message)) {
                localRequests_.erase(*id);
                return false;
            }
            return true;
        }

        if (kind == protocol::JsonRpcKind::Response) {
            if (!id) {
                return false;
            }
            const auto owner = appServerRequestOwners_.find(*id);
            if (owner == appServerRequestOwners_.end() || owner->second.kind != ServerRequestOwnerKind::LocalApplication) {
                return false;
            }
            if (!sendToAppServer(message)) {
                return false;
            }
            appServerRequestOwners_.erase(owner);
            return true;
        }

        return sendToAppServer(message);
    }

    void CodexBridge::onRawJson(RawHandler handler) {
        rawHandler_ = std::move(handler);
    }

    std::optional<std::string> CodexBridge::controllerConnectionId() const {
        return controller_;
    }

    std::size_t CodexBridge::frontendCount() const noexcept {
        return frontends_.size();
    }

    std::uint64_t CodexBridge::providerGeneration() const noexcept {
        return providerGeneration_;
    }

    bool CodexBridge::appServerReady() const noexcept {
        return appServerReady_;
    }

    void CodexBridge::handleAppServerEnvelope(std::string_view connectionId, const nlohmann::json& message) {
        const auto payload = message.find("payload");
        if (payload == message.end()) {
            reject(connectionId, nlohmann::json::object(), -32600, "appserver envelope is missing payload");
            return;
        }
        const protocol::JsonRpcKind kind = protocol::classifyJsonRpc(*payload);
        const std::optional<std::string> id = protocol::jsonRpcIdKey(*payload);
        if (kind == protocol::JsonRpcKind::Invalid) {
            reject(connectionId, *payload, -32600, "payload is not a valid JSON-RPC request, notification, or response");
            return;
        }

        if (kind == protocol::JsonRpcKind::Response) {
            if (!id) {
                reject(connectionId, *payload, -32600, "app-server response is missing an id");
                return;
            }
            const auto owner = appServerRequestOwners_.find(*id);
            if (owner == appServerRequestOwners_.end() || owner->second.kind != ServerRequestOwnerKind::Frontend ||
                owner->second.frontendConnectionId != connectionId) {
                reject(connectionId, *payload, -32004, "response does not match a server request delivered to this frontend");
                return;
            }
            if (!sendToAppServer(*payload)) {
                appServerRequestOwners_.erase(owner);
                reject(connectionId, *payload, -32005, "app-server transport rejected the response");
                return;
            }
            appServerRequestOwners_.erase(owner);
            return;
        }

        const std::optional<std::string> method = protocol::jsonRpcMethod(*payload);
        if (method && (*method == "initialize" || *method == "initialized")) {
            reject(connectionId, *payload, -32003, "provider initialize handshake is owned by codex-bridge");
            return;
        }

        if (!mayForward(connectionId, *payload)) {
            reject(connectionId, *payload, -32001, "observer is not allowed to send this app-server message");
            return;
        }
        if (appServer_ == nullptr || !appServer_->isConnected() || !appServerReady_) {
            reject(connectionId, *payload, -32002, "app-server session is not ready");
            return;
        }

        if (kind == protocol::JsonRpcKind::Request) {
            if (!id) {
                reject(connectionId, *payload, -32600, "frontend request is missing an id");
                return;
            }
            nlohmann::json forwarded = *payload;
            const std::string upstreamId = nextFrontendRequestId(connectionId);
            const std::string upstreamKey = nlohmann::json(upstreamId).dump();
            frontendRequestOwners_.emplace(upstreamKey, FrontendRequest{std::string(connectionId), (*payload)["id"]});
            forwarded["id"] = upstreamId;
            if (!sendToAppServer(forwarded)) {
                frontendRequestOwners_.erase(upstreamKey);
                reject(connectionId, *payload, -32005, "app-server transport rejected the message");
            }
            return;
        }

        if (!sendToAppServer(*payload)) {
            reject(connectionId, *payload, -32005, "app-server transport rejected the message");
        }
    }

    void CodexBridge::handleControllerCommand(std::string_view connectionId, const nlohmann::json& message) {
        const std::string action = message.value("action", std::string{});
        if (action == "claim") {
            if (!controller_ || *controller_ == connectionId) {
                setController(std::string(connectionId));
            } else {
                emitDiagnostic("controller-claim-rejected",
                               "another frontend is already controller",
                               connectionId,
                               {{"controllerConnectionId", *controller_}});
            }
            return;
        }
        if (action == "release") {
            if (controller_ && *controller_ == connectionId) {
                setController(std::nullopt);
            } else {
                emitDiagnostic("controller-release-rejected", "frontend is not the controller", connectionId);
            }
            return;
        }
        if (action == "transfer") {
            if (!controller_ || *controller_ != connectionId) {
                emitDiagnostic("controller-transfer-rejected", "only the controller may transfer control", connectionId);
                return;
            }
            const std::string target = message.value("targetConnectionId", std::string{});
            if (!frontends_.contains(target)) {
                emitDiagnostic(
                    "controller-transfer-rejected", "target frontend does not exist", connectionId, {{"targetConnectionId", target}});
                return;
            }
            setController(target);
            return;
        }
        emitDiagnostic("controller-command-invalid", "controller command requires claim, release, or transfer action", connectionId);
    }

    bool CodexBridge::mayForward(std::string_view connectionId, const nlohmann::json& payload) const {
        const auto frontend = frontends_.find(std::string(connectionId));
        if (frontend == frontends_.end()) {
            return false;
        }
        if (frontend->second.role == protocol::Role::Controller) {
            return true;
        }
        if (!options_.observersMayRead || protocol::classifyJsonRpc(payload) != protocol::JsonRpcKind::Request) {
            return false;
        }
        const std::optional<std::string> method = protocol::jsonRpcMethod(payload);
        return method && observerMethodIsReadOnly(*method);
    }

    bool CodexBridge::observerMethodIsReadOnly(std::string_view method) const {
        static constexpr std::array exactMethods{
            std::string_view{"account/read"},
            std::string_view{"account/rateLimits/read"},
            std::string_view{"account/usage/read"},
            std::string_view{"account/workspaceMessages/read"},
            std::string_view{"app/installed"},
            std::string_view{"app/list"},
            std::string_view{"app/read"},
            std::string_view{"config/read"},
            std::string_view{"configRequirements/read"},
            std::string_view{"experimentalFeature/list"},
            std::string_view{"fs/getMetadata"},
            std::string_view{"fs/readDirectory"},
            std::string_view{"fs/readFile"},
            std::string_view{"hooks/list"},
            std::string_view{"mcpServer/resource/read"},
            std::string_view{"mcpServerStatus/list"},
            std::string_view{"model/list"},
            std::string_view{"modelProvider/capabilities/read"},
            std::string_view{"permissionProfile/list"},
            std::string_view{"plugin/installed"},
            std::string_view{"plugin/list"},
            std::string_view{"plugin/read"},
            std::string_view{"plugin/share/list"},
            std::string_view{"plugin/skill/read"},
            std::string_view{"skills/list"},
            std::string_view{"thread/goal/get"},
            std::string_view{"thread/list"},
            std::string_view{"thread/loaded/list"},
            std::string_view{"thread/read"},
            std::string_view{"threadSection/list"},
        };
        return std::ranges::find(exactMethods, method) != exactMethods.end();
    }

    void CodexBridge::reject(std::string_view connectionId, const nlohmann::json& payload, int code, std::string_view reason) {
        if (payload.is_object() && payload.contains("id") && !payload["id"].is_null()) {
            const auto frontend = frontends_.find(std::string(connectionId));
            if (frontend != frontends_.end()) {
                sendTo(connectionId,
                       protocol::appServerEnvelope(
                           connectionId, frontend->second.role, nextSequence(), protocol::jsonRpcError(payload["id"], code, reason)));
                return;
            }
        }
        emitDiagnostic("frontend-message-rejected", reason, connectionId, {{"errorCode", code}});
    }

    void CodexBridge::emitDiagnostic(std::string_view code,
                                     std::string_view message,
                                     std::optional<std::string_view> connectionId,
                                     nlohmann::json details) {
        const nlohmann::json event = protocol::diagnosticEvent(code, message, connectionId, nextSequence(), std::move(details));
        if (connectionId) {
            sendTo(*connectionId, event);
        } else {
            broadcast(event);
        }
    }

    void CodexBridge::setController(std::optional<std::string> connectionId) {
        if (controller_) {
            const auto previous = frontends_.find(*controller_);
            if (previous != frontends_.end()) {
                previous->second.role = protocol::Role::Observer;
            }
        }
        controller_ = std::move(connectionId);
        if (controller_) {
            frontends_.at(*controller_).role = protocol::Role::Controller;
        }
        broadcastController();
    }

    void CodexBridge::sendTo(std::string_view connectionId, const nlohmann::json& message) {
        const auto frontend = frontends_.find(std::string(connectionId));
        if (frontend != frontends_.end() && !frontend->second.endpoint->send(message)) {
            frontend->second.endpoint->close("outbound bridge message rejected");
        }
    }

    void CodexBridge::broadcast(const nlohmann::json& message) {
        std::vector<std::string> connectionIds;
        connectionIds.reserve(frontends_.size());
        for (const auto& [connectionId, record] : frontends_) {
            static_cast<void>(record);
            connectionIds.push_back(connectionId);
        }
        for (const std::string& connectionId : connectionIds) {
            sendTo(connectionId, message);
        }
    }

    void CodexBridge::broadcastController() {
        const std::optional<std::string_view> id = controller_ ? std::optional<std::string_view>(*controller_) : std::nullopt;
        broadcast(protocol::controllerEvent(id, nextSequence()));
    }

    bool CodexBridge::sendToAppServer(const nlohmann::json& message) {
        if (appServer_ == nullptr || !appServer_->isConnected() || !appServer_->send(message)) {
            return false;
        }
        if (rawHandler_) {
            rawHandler_(protocol::AppServerDirection::ToAppServer, message);
        }
        return true;
    }

    std::string CodexBridge::requestTyped(std::string_view method, std::optional<nlohmann::json> params, LocalResponseHandler handler) {
        const std::string id = nextLocalRequestId();
        nlohmann::json request{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
        if (params) {
            request["params"] = std::move(*params);
        }

        const nlohmann::json nativeId = id;
        const std::string key = nativeId.dump();
        localRequests_.emplace(key, LocalRequest{nativeId, std::move(handler)});
        if (!sendToAppServer(request)) {
            const auto pending = localRequests_.find(key);
            if (pending != localRequests_.end()) {
                LocalResponseHandler failed = std::move(pending->second.handler);
                const nlohmann::json failedId = pending->second.id;
                localRequests_.erase(pending);
                if (failed) {
                    failed(protocol::jsonRpcError(failedId, -32002, "app-server transport is unavailable"));
                }
            }
        }
        return id;
    }

    bool CodexBridge::sendLocalNotification(std::string_view method, std::optional<nlohmann::json> params) {
        nlohmann::json notification{{"jsonrpc", "2.0"}, {"method", method}};
        if (params) {
            notification["params"] = std::move(*params);
        }
        return sendToAppServer(notification);
    }

    bool CodexBridge::sendServerResponse(const nlohmann::json& id, const nlohmann::json& result) {
        if (id.is_null()) {
            return false;
        }
        return sendRawJson({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
    }

    bool CodexBridge::sendServerError(const nlohmann::json& id, int code, std::string_view message, nlohmann::json data) {
        return id.is_null() ? false : sendRawJson(protocol::jsonRpcError(id, code, message, std::move(data)));
    }

    void CodexBridge::registerServerRequestHandler(std::string_view method, LocalEventHandler handler) {
        if (handler) {
            serverRequestHandlers_[std::string(method)] = std::move(handler);
        } else {
            serverRequestHandlers_.erase(std::string(method));
        }
    }

    void CodexBridge::registerServerNotificationHandler(std::string_view method, LocalEventHandler handler) {
        if (handler) {
            serverNotificationHandlers_[std::string(method)] = std::move(handler);
        } else {
            serverNotificationHandlers_.erase(std::string(method));
        }
    }

    void CodexBridge::failFrontendServerRequests(std::string_view connectionId, std::string_view reason) {
        std::vector<std::pair<std::string, nlohmann::json>> failed;
        for (const auto& [key, owner] : appServerRequestOwners_) {
            if (owner.kind == ServerRequestOwnerKind::Frontend && owner.frontendConnectionId == connectionId) {
                failed.emplace_back(key, owner.id);
            }
        }
        for (const auto& [key, id] : failed) {
            sendToAppServer(protocol::jsonRpcError(id, -32011, reason));
            appServerRequestOwners_.erase(key);
        }
    }

    void CodexBridge::failFrontendRequests(std::string_view reason) {
        auto requests = std::move(frontendRequestOwners_);
        frontendRequestOwners_.clear();
        for (const auto& [key, request] : requests) {
            static_cast<void>(key);
            const auto frontend = frontends_.find(request.connectionId);
            if (frontend != frontends_.end()) {
                sendTo(request.connectionId,
                       protocol::appServerEnvelope(request.connectionId,
                                                   frontend->second.role,
                                                   nextSequence(),
                                                   protocol::jsonRpcError(request.frontendId, -32002, reason)));
            }
        }
    }

    void CodexBridge::failLocalRequests(std::string_view reason) {
        auto pending = std::move(localRequests_);
        localRequests_.clear();
        for (auto& [key, request] : pending) {
            static_cast<void>(key);
            if (request.handler) {
                request.handler(protocol::jsonRpcError(request.id, -32002, reason));
            }
        }
    }

    std::uint64_t CodexBridge::nextSequence() noexcept {
        return ++sequence_;
    }

    std::string CodexBridge::nextLocalRequestId() {
        std::string id;
        do {
            id = "codex-local-" + std::to_string(providerGeneration_) + '-' + std::to_string(nextLocalRequestNumber_++);
        } while (localRequests_.contains(nlohmann::json(id).dump()) || frontendRequestOwners_.contains(nlohmann::json(id).dump()));
        return id;
    }

    std::string CodexBridge::nextFrontendRequestId(std::string_view connectionId) {
        std::string id;
        std::string key;
        do {
            id = "codex-remote-" + std::to_string(providerGeneration_) + '-' + std::string(connectionId) + '-' +
                 std::to_string(nextFrontendRequestNumber_++);
            key = nlohmann::json(id).dump();
        } while (frontendRequestOwners_.contains(key) || localRequests_.contains(key));
        return id;
    }

} // namespace ai::openai::codex::bridge
