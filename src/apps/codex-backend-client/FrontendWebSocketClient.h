/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_CLIENT_FRONTENDWEBSOCKETCLIENT_H
#define APPS_CODEX_BACKEND_CLIENT_FRONTENDWEBSOCKETCLIENT_H

#include "ai/openai/codex/frontend/client/Client.h"

#include <cstdint>
#include <functional>
#include <string>

namespace core::socket::stream {
    class SocketConnection;
}

namespace apps::codex_backend_client {

    struct FrontendWebSocketClientCallbacks {
        std::function<void()> onConnected;
        std::function<void()> onDisconnected;
        std::function<void(std::string)> onFailure;
        std::function<void(std::uint64_t)> onAttemptConnected = {};
        std::function<void(std::uint64_t)> onAttemptDisconnected = {};
        std::function<void(std::uint64_t, std::string)> onAttemptFailure = {};
        std::function<void(bool)> onBeforeTransportConnected;
        std::function<void()> onLocalShutdown = {};
    };

    class FrontendWebSocketClientRuntime {
    public:
        FrontendWebSocketClientRuntime(ai::openai::codex::frontend::client::Client& client,
                                       FrontendWebSocketClientCallbacks callbacks = {});
        FrontendWebSocketClientRuntime(const FrontendWebSocketClientRuntime&) = delete;
        FrontendWebSocketClientRuntime& operator=(const FrontendWebSocketClientRuntime&) = delete;
        ~FrontendWebSocketClientRuntime();

        [[nodiscard]] bool install() noexcept;
        void uninstall() noexcept;
        // Closes an active WebSocket because the application is terminating.
        // Remote/SDK connection failures follow the subprotocol close path.
        void shutdown() noexcept;
        [[nodiscard]] bool connected() const noexcept;
        void reportFailure(std::string message) noexcept;
        [[nodiscard]] bool prepareAttempt(std::uint64_t generation) noexcept;
        [[nodiscard]] bool bindAttemptTransport(std::uint64_t generation, const core::socket::stream::SocketConnection* transport) noexcept;
        void abandonAttempt(std::uint64_t generation) noexcept;
        [[nodiscard]] bool isCurrentAttempt(std::uint64_t generation) const noexcept;
        void reportAttemptFailure(std::uint64_t generation, std::string message) noexcept;

    private:
        friend class FrontendWebSocketClientSubProtocol;
        friend struct FrontendWebSocketClientRuntimeTestAccess;

        [[nodiscard]] std::uint64_t claimAttempt(const core::socket::stream::SocketConnection* transport) noexcept;

        ai::openai::codex::frontend::client::Client& client;
        FrontendWebSocketClientCallbacks callbacks;
        class FrontendWebSocketClientSubProtocol* active = nullptr;
        std::uint64_t preparedGeneration = 0;
        const core::socket::stream::SocketConnection* preparedTransport = nullptr;
        std::uint64_t nextImplicitGeneration = 1;
        bool installed = false;
    };

    void linkFrontendWebSocketClient() noexcept;

} // namespace apps::codex_backend_client

#endif // APPS_CODEX_BACKEND_CLIENT_FRONTENDWEBSOCKETCLIENT_H
