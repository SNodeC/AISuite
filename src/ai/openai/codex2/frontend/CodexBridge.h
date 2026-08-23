/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX2_FRONTEND_CODEXBRIDGE_H
#define AI_OPENAI_CODEX2_FRONTEND_CODEXBRIDGE_H

#include "ai/openai/codex2/protocol/Envelope.h"
#include "ai/openai/codex2/protocol/generated/ProtocolTypes.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

namespace ai::openai::codex2::frontend {

    class CodexBridge {
    public:
        using Sender = std::function<bool(const nlohmann::json&)>;
        using RawHandler = std::function<void(protocol::AppServerDirection, const nlohmann::json&)>;
        using BridgeEventHandler = std::function<void(const nlohmann::json&)>;

        template <typename Operation>
        using ResponseHandler = std::function<void(typename Operation::Response&)>;

        template <typename Operation>
        using EventHandler = std::function<void(typename Operation::Params&)>;

        explicit CodexBridge(Sender sender);

        void setSender(Sender sender);

        bool receive(const nlohmann::json& bridgeMessage);
        bool sendRawJson(const nlohmann::json& appServerMessage);
        void onRawJson(RawHandler handler);
        void onBridgeEvent(BridgeEventHandler handler);
        void transportDisconnected(std::string_view reason);

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
            return sendNotification(Operation::method, std::make_optional(nlohmann::json(params.getPayload())));
        }

        template <typename Operation>
            requires(!Operation::paramsRequired)
        bool notify() {
            return sendNotification(Operation::method, std::nullopt);
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

#define AI_OPENAI_CODEX2_DECLARE_CLIENT_REQUEST(OperationName, methodName)                                                     \
    std::string methodName(                                                                                                   \
        const generated::client_requests::OperationName::Params& params,                                                      \
        ResponseHandler<generated::client_requests::OperationName> handler) {                                                 \
        return request<generated::client_requests::OperationName>(params, std::move(handler));                                \
    }
        AI_OPENAI_CODEX2_CLIENT_REQUESTS(AI_OPENAI_CODEX2_DECLARE_CLIENT_REQUEST)
#undef AI_OPENAI_CODEX2_DECLARE_CLIENT_REQUEST

#define AI_OPENAI_CODEX2_DECLARE_PARAMETERLESS_CLIENT_REQUEST(OperationName, methodName)                                      \
    std::string methodName(ResponseHandler<generated::client_requests::OperationName> handler) {                              \
        return request<generated::client_requests::OperationName>(std::move(handler));                                        \
    }
        AI_OPENAI_CODEX2_PARAMETERLESS_CLIENT_REQUESTS(AI_OPENAI_CODEX2_DECLARE_PARAMETERLESS_CLIENT_REQUEST)
#undef AI_OPENAI_CODEX2_DECLARE_PARAMETERLESS_CLIENT_REQUEST

#define AI_OPENAI_CODEX2_DECLARE_CLIENT_NOTIFICATION(OperationName, methodName)                                               \
    bool methodName(const generated::client_notifications::OperationName::Params& params) {                                  \
        return notify<generated::client_notifications::OperationName>(params);                                                \
    }
        AI_OPENAI_CODEX2_CLIENT_NOTIFICATIONS(AI_OPENAI_CODEX2_DECLARE_CLIENT_NOTIFICATION)
#undef AI_OPENAI_CODEX2_DECLARE_CLIENT_NOTIFICATION

#define AI_OPENAI_CODEX2_DECLARE_PARAMETERLESS_CLIENT_NOTIFICATION(OperationName, methodName)                                 \
    bool methodName() {                                                                                                       \
        return notify<generated::client_notifications::OperationName>();                                                      \
    }
        AI_OPENAI_CODEX2_PARAMETERLESS_CLIENT_NOTIFICATIONS(AI_OPENAI_CODEX2_DECLARE_PARAMETERLESS_CLIENT_NOTIFICATION)
#undef AI_OPENAI_CODEX2_DECLARE_PARAMETERLESS_CLIENT_NOTIFICATION

#define AI_OPENAI_CODEX2_DECLARE_SERVER_REQUEST_HANDLER(OperationName, methodName)                                            \
    void on##OperationName(EventHandler<generated::server_requests::OperationName> handler) {                                 \
        onServerRequest<generated::server_requests::OperationName>(std::move(handler));                                       \
    }
        AI_OPENAI_CODEX2_SERVER_REQUESTS(AI_OPENAI_CODEX2_DECLARE_SERVER_REQUEST_HANDLER)
#undef AI_OPENAI_CODEX2_DECLARE_SERVER_REQUEST_HANDLER

#define AI_OPENAI_CODEX2_DECLARE_SERVER_REQUEST_RESPONSE(OperationName, methodName)                                          \
    bool respondTo##OperationName(                                                                                           \
        const generated::server_requests::OperationName::Params& request,                                                    \
        const generated::server_requests::OperationName::Response& response) {                                               \
        return respond<generated::server_requests::OperationName>(request, response);                                        \
    }
        AI_OPENAI_CODEX2_SERVER_REQUESTS(AI_OPENAI_CODEX2_DECLARE_SERVER_REQUEST_RESPONSE)
#undef AI_OPENAI_CODEX2_DECLARE_SERVER_REQUEST_RESPONSE

#define AI_OPENAI_CODEX2_DECLARE_SERVER_NOTIFICATION_HANDLER(OperationName, methodName)                                       \
    void on##OperationName(EventHandler<generated::server_notifications::OperationName> handler) {                            \
        onServerNotification<generated::server_notifications::OperationName>(std::move(handler));                            \
    }
        AI_OPENAI_CODEX2_SERVER_NOTIFICATIONS(AI_OPENAI_CODEX2_DECLARE_SERVER_NOTIFICATION_HANDLER)
#undef AI_OPENAI_CODEX2_DECLARE_SERVER_NOTIFICATION_HANDLER

        bool claimController();
        bool releaseController();
        bool transferController(std::string targetConnectionId);

        const std::optional<std::string>& connectionId() const noexcept;
        const std::optional<std::string>& controllerConnectionId() const noexcept;
        std::optional<protocol::Role> role() const noexcept;
        bool isController() const noexcept;

    private:
        using PendingHandler = std::function<void(nlohmann::json)>;
        using EventDispatcher = std::function<void(nlohmann::json)>;

        std::string requestTyped(std::string_view method,
                                 std::optional<nlohmann::json> params,
                                 PendingHandler handler);
        bool sendNotification(std::string_view method, std::optional<nlohmann::json> params);
        bool sendServerResponse(const nlohmann::json& id, const nlohmann::json& result);
        bool sendServerError(const nlohmann::json& id, int code, std::string_view message, nlohmann::json data);
        bool sendAppServerMessage(const nlohmann::json& message);
        bool sendBridgeCommand(nlohmann::json command);
        void registerServerRequestHandler(std::string_view method, EventDispatcher handler);
        void registerServerNotificationHandler(std::string_view method, EventDispatcher handler);
        void updateBridgeState(const nlohmann::json& message);
        std::string nextRequestId();

        Sender sender_;
        RawHandler rawHandler_;
        BridgeEventHandler bridgeEventHandler_;
        std::unordered_map<std::string, PendingHandler> pending_;
        std::unordered_map<std::string, EventDispatcher> serverRequestHandlers_;
        std::unordered_map<std::string, EventDispatcher> serverNotificationHandlers_;
        std::optional<std::string> connectionId_;
        std::optional<std::string> controllerConnectionId_;
        std::optional<protocol::Role> role_;
        std::uint64_t nextRequestNumber_ = 1;
    };

} // namespace ai::openai::codex2::frontend

#endif
