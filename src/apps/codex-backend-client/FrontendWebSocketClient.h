/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_CLIENT_FRONTENDWEBSOCKETCLIENT_H
#define APPS_CODEX_BACKEND_CLIENT_FRONTENDWEBSOCKETCLIENT_H

#include "ai/openai/codex/frontend/client/Client.h"

#include <functional>
#include <string>

namespace apps::codex_backend_client {

    struct FrontendWebSocketClientCallbacks {
        std::function<void()> onConnected;
        std::function<void()> onDisconnected;
        std::function<void(std::string)> onFailure;
        std::function<void(bool)> onBeforeTransportConnected;
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
        void disconnect() noexcept;
        [[nodiscard]] bool connected() const noexcept;
        void reportFailure(std::string message) noexcept;

    private:
        friend class FrontendWebSocketClientSubProtocol;

        ai::openai::codex::frontend::client::Client& client;
        FrontendWebSocketClientCallbacks callbacks;
        class FrontendWebSocketClientSubProtocol* active = nullptr;
        bool installed = false;
    };

    void linkFrontendWebSocketClient() noexcept;

} // namespace apps::codex_backend_client

#endif // APPS_CODEX_BACKEND_CLIENT_FRONTENDWEBSOCKETCLIENT_H
