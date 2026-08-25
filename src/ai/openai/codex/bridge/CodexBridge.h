/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_BRIDGE_CODEXBRIDGE_H
#define AI_OPENAI_CODEX_BRIDGE_CODEXBRIDGE_H

#include "ai/openai/codex/bridge/Endpoint.h"
#include "ai/openai/codex/protocol/Envelope.h"
#include "ai/openai/codex/protocol/generated/ProtocolTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

namespace ai::openai::codex::bridge {

    struct CodexBridgeOptions {
        bool firstFrontendBecomesController = true;
        bool observersMayRead = true;
        std::size_t maximumFrontends = 16;
    };

    class CodexBridge {
    public:
        using RawHandler = std::function<void(protocol::AppServerDirection, const nlohmann::json&)>;
        using ProviderLifecycleHandler = std::function<void(bool)>;

        template <typename Operation>
        using ResponseHandler = std::function<void(typename Operation::Response&)>;

        template <typename Operation>
        using EventHandler = std::function<void(typename Operation::Params&)>;

        explicit CodexBridge(CodexBridgeOptions options = {});

        void setAppServer(AppServerEndpoint* appServer);
        std::string registerFrontend(FrontendEndpoint& frontend);
        void unregisterFrontend(std::string_view connectionId);

        void receiveFromFrontend(std::string_view connectionId, const nlohmann::json& message);
        void receiveFromAppServer(const nlohmann::json& message);
        void appServerConnected();
        void appServerDisconnected(std::string_view reason);
        void setAppServerReady();
        void onProviderLifecycle(ProviderLifecycleHandler handler);

        bool sendRawJson(const nlohmann::json& message);
        void onRawJson(RawHandler handler);

        template <typename Operation>
        std::string request(const typename Operation::Params& params, ResponseHandler<Operation> handler) {
            return requestTyped(Operation::method,
                                std::make_optional(nlohmann::json(params.getPayload())),
                                [handler = std::move(handler)](nlohmann::json message) mutable {
                                    typename Operation::Response response(std::move(message));
                                    if (handler) {
                                        handler(response);
                                    }
                                });
        }

        template <typename Operation>
            requires(!Operation::paramsRequired)
        std::string request(ResponseHandler<Operation> handler) {
            return requestTyped(Operation::method,
                                std::nullopt,
                                [handler = std::move(handler)](nlohmann::json message) mutable {
                                    typename Operation::Response response(std::move(message));
                                    if (handler) {
                                        handler(response);
                                    }
                                });
        }

        template <typename Operation>
        bool notify(const typename Operation::Params& params) {
            return sendLocalNotification(Operation::method, std::make_optional(nlohmann::json(params.getPayload())));
        }

        template <typename Operation>
            requires(!Operation::paramsRequired)
        bool notify() {
            return sendLocalNotification(Operation::method, std::nullopt);
        }

        template <typename Operation>
        void onServerRequest(EventHandler<Operation> handler) {
            registerServerRequestHandler(
                Operation::method,
                [handler = std::move(handler)](nlohmann::json message) mutable {
                    typename Operation::Params request(std::move(message));
                    if (handler) {
                        handler(request);
                    }
                });
        }

        template <typename Operation>
        void onServerNotification(EventHandler<Operation> handler) {
            registerServerNotificationHandler(
                Operation::method,
                [handler = std::move(handler)](nlohmann::json message) mutable {
                    typename Operation::Params notification(std::move(message));
                    if (handler) {
                        handler(notification);
                    }
                });
        }

        template <typename Operation>
        bool respond(const typename Operation::Params& request, const typename Operation::Response& response) {
            return sendServerResponse(request.jsonRpcId(), response.getPayload());
        }

        template <typename Operation>
        bool respondError(const typename Operation::Params& request,
                          int code,
                          std::string_view message,
                          nlohmann::json data = nullptr) {
            return sendServerError(request.jsonRpcId(), code, message, std::move(data));
        }

#define AI_OPENAI_CODEX_DECLARE_CLIENT_REQUEST(OperationName, methodName)                                                     \
    std::string methodName(                                                                                                   \
        const generated::client_requests::OperationName::Params& params,                                                      \
        ResponseHandler<generated::client_requests::OperationName> handler) {                                                 \
        return request<generated::client_requests::OperationName>(params, std::move(handler));                                \
    }
        AI_OPENAI_CODEX_CLIENT_REQUESTS(AI_OPENAI_CODEX_DECLARE_CLIENT_REQUEST)
#undef AI_OPENAI_CODEX_DECLARE_CLIENT_REQUEST

#define AI_OPENAI_CODEX_DECLARE_PARAMETERLESS_CLIENT_REQUEST(OperationName, methodName)                                      \
    std::string methodName(ResponseHandler<generated::client_requests::OperationName> handler) {                              \
        return request<generated::client_requests::OperationName>(std::move(handler));                                        \
    }
        AI_OPENAI_CODEX_PARAMETERLESS_CLIENT_REQUESTS(AI_OPENAI_CODEX_DECLARE_PARAMETERLESS_CLIENT_REQUEST)
#undef AI_OPENAI_CODEX_DECLARE_PARAMETERLESS_CLIENT_REQUEST

#define AI_OPENAI_CODEX_DECLARE_CLIENT_NOTIFICATION(OperationName, methodName)                                               \
    bool methodName(const generated::client_notifications::OperationName::Params& params) {                                  \
        return notify<generated::client_notifications::OperationName>(params);                                                \
    }
        AI_OPENAI_CODEX_CLIENT_NOTIFICATIONS(AI_OPENAI_CODEX_DECLARE_CLIENT_NOTIFICATION)
#undef AI_OPENAI_CODEX_DECLARE_CLIENT_NOTIFICATION

#define AI_OPENAI_CODEX_DECLARE_PARAMETERLESS_CLIENT_NOTIFICATION(OperationName, methodName)                                 \
    bool methodName() {                                                                                                       \
        return notify<generated::client_notifications::OperationName>();                                                      \
    }
        AI_OPENAI_CODEX_PARAMETERLESS_CLIENT_NOTIFICATIONS(AI_OPENAI_CODEX_DECLARE_PARAMETERLESS_CLIENT_NOTIFICATION)
#undef AI_OPENAI_CODEX_DECLARE_PARAMETERLESS_CLIENT_NOTIFICATION

#define AI_OPENAI_CODEX_DECLARE_SERVER_REQUEST_HANDLER(OperationName, methodName)                                            \
    void on##OperationName(EventHandler<generated::server_requests::OperationName> handler) {                                 \
        onServerRequest<generated::server_requests::OperationName>(std::move(handler));                                       \
    }
        AI_OPENAI_CODEX_SERVER_REQUESTS(AI_OPENAI_CODEX_DECLARE_SERVER_REQUEST_HANDLER)
#undef AI_OPENAI_CODEX_DECLARE_SERVER_REQUEST_HANDLER

#define AI_OPENAI_CODEX_DECLARE_SERVER_REQUEST_RESPONSE(OperationName, methodName)                                          \
    bool respondTo##OperationName(                                                                                           \
        const generated::server_requests::OperationName::Params& request,                                                    \
        const generated::server_requests::OperationName::Response& response) {                                               \
        return respond<generated::server_requests::OperationName>(request, response);                                        \
    }
        AI_OPENAI_CODEX_SERVER_REQUESTS(AI_OPENAI_CODEX_DECLARE_SERVER_REQUEST_RESPONSE)
#undef AI_OPENAI_CODEX_DECLARE_SERVER_REQUEST_RESPONSE

#define AI_OPENAI_CODEX_DECLARE_SERVER_NOTIFICATION_HANDLER(OperationName, methodName)                                       \
    void on##OperationName(EventHandler<generated::server_notifications::OperationName> handler) {                            \
        onServerNotification<generated::server_notifications::OperationName>(std::move(handler));                            \
    }
        AI_OPENAI_CODEX_SERVER_NOTIFICATIONS(AI_OPENAI_CODEX_DECLARE_SERVER_NOTIFICATION_HANDLER)
#undef AI_OPENAI_CODEX_DECLARE_SERVER_NOTIFICATION_HANDLER

        std::optional<std::string> controllerConnectionId() const;
        std::size_t frontendCount() const noexcept;
        std::uint64_t providerGeneration() const noexcept;
        bool appServerReady() const noexcept;

    private:
        using LocalResponseHandler = std::function<void(nlohmann::json)>;
        using LocalEventHandler = std::function<void(nlohmann::json)>;

        enum class ServerRequestOwnerKind { LocalApplication, Frontend };

        struct FrontendRecord {
            FrontendEndpoint* endpoint;
            protocol::Role role;
        };

        struct ServerRequestOwner {
            ServerRequestOwnerKind kind;
            std::string frontendConnectionId;
            nlohmann::json id;
        };

        struct LocalRequest {
            nlohmann::json id;
            LocalResponseHandler handler;
        };

        struct FrontendRequest {
            std::string connectionId;
            nlohmann::json frontendId;
            std::string frontendKey;
        };

        void handleAppServerEnvelope(std::string_view connectionId, const nlohmann::json& message);
        void handleControllerCommand(std::string_view connectionId, const nlohmann::json& message);
        bool mayForward(std::string_view connectionId, const nlohmann::json& payload) const;
        bool observerMethodIsReadOnly(std::string_view method) const;
        void reject(std::string_view connectionId, const nlohmann::json& payload, int code, std::string_view reason);
        void emitDiagnostic(std::string_view code,
                            std::string_view message,
                            std::optional<std::string_view> connectionId,
                            nlohmann::json details = nlohmann::json::object());
        void setController(std::optional<std::string> connectionId);
        void sendTo(std::string_view connectionId, const nlohmann::json& message);
        void broadcast(const nlohmann::json& message);
        void broadcastController();
        bool sendToAppServer(const nlohmann::json& message);
        std::string requestTyped(std::string_view method,
                                 std::optional<nlohmann::json> params,
                                 LocalResponseHandler handler);
        bool sendLocalNotification(std::string_view method, std::optional<nlohmann::json> params);
        bool sendServerResponse(const nlohmann::json& id, const nlohmann::json& result);
        bool sendServerError(const nlohmann::json& id, int code, std::string_view message, nlohmann::json data);
        void registerServerRequestHandler(std::string_view method, LocalEventHandler handler);
        void registerServerNotificationHandler(std::string_view method, LocalEventHandler handler);
        void failFrontendServerRequests(std::string_view connectionId, std::string_view reason);
        void failFrontendRequests(std::string_view reason);
        void failLocalRequests(std::string_view reason);
        std::uint64_t nextSequence() noexcept;
        std::string nextLocalRequestId();
        std::string nextFrontendRequestId(std::string_view connectionId);

        CodexBridgeOptions options_;
        AppServerEndpoint* appServer_ = nullptr;
        RawHandler rawHandler_;
        ProviderLifecycleHandler providerLifecycleHandler_;
        std::unordered_map<std::string, FrontendRecord> frontends_;
        std::optional<std::string> controller_;
        std::unordered_map<std::string, FrontendRequest> frontendRequestOwners_;
        std::unordered_map<std::string, std::string> frontendRequestKeys_;
        std::unordered_map<std::string, LocalRequest> localRequests_;
        std::unordered_map<std::string, ServerRequestOwner> appServerRequestOwners_;
        std::unordered_map<std::string, LocalEventHandler> serverRequestHandlers_;
        std::unordered_map<std::string, LocalEventHandler> serverNotificationHandlers_;
        std::uint64_t nextConnectionNumber_ = 1;
        std::uint64_t nextLocalRequestNumber_ = 1;
        std::uint64_t nextFrontendRequestNumber_ = 1;
        std::uint64_t sequence_ = 0;
        std::uint64_t providerGeneration_ = 0;
        bool appServerReady_ = false;
    };

} // namespace ai::openai::codex::bridge

#endif
