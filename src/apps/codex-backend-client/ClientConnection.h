/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_CLIENT_CLIENTCONNECTION_H
#define APPS_CODEX_BACKEND_CLIENT_CLIENTCONNECTION_H

#include "ai/openai/codex/frontend/client/Client.h"

#include <functional>
#include <string>

namespace apps::codex_backend_client {

    class CodexBackendClientSocketContext;

    struct ClientConnectionCallbacks {
        std::function<void()> onConnected;
        std::function<void()> onDisconnected;
        std::function<void(std::string)> onFailure;
        std::function<void(const ai::openai::codex::frontend::client::OutboundMessage&)> onOutbound;
        bool verifiedLocalUnix = false;
        std::function<void(bool)> onBeforeTransportConnected;
    };

    class ClientConnection {
    public:
        ClientConnection(ai::openai::codex::frontend::client::Client& sdk, ClientConnectionCallbacks callbacks = {});
        ClientConnection(const ClientConnection&) = delete;
        ClientConnection(ClientConnection&&) = delete;
        ClientConnection& operator=(const ClientConnection&) = delete;
        ClientConnection& operator=(ClientConnection&&) = delete;
        ~ClientConnection();

        void disconnect() noexcept;
        [[nodiscard]] bool connected() const noexcept;

    private:
        friend class CodexBackendClientSocketContext;

        void attach(CodexBackendClientSocketContext& context) noexcept;
        void didConnect(CodexBackendClientSocketContext& context) noexcept;
        void didReceive(CodexBackendClientSocketContext& context, std::string frame) noexcept;
        void didFail(CodexBackendClientSocketContext& context, std::string message) noexcept;
        void detach(CodexBackendClientSocketContext& context) noexcept;
        [[nodiscard]] ai::openai::codex::frontend::client::SendResult
        send(ai::openai::codex::frontend::client::OutboundMessage message) noexcept;
        void closeTransport(std::string reason) noexcept;

        ai::openai::codex::frontend::client::Client& sdk;
        ClientConnectionCallbacks callbacks;
        ai::openai::codex::frontend::client::Connection protocolConnection;
        CodexBackendClientSocketContext* context = nullptr;
        bool online = false;
    };

} // namespace apps::codex_backend_client

#endif // APPS_CODEX_BACKEND_CLIENT_CLIENTCONNECTION_H
