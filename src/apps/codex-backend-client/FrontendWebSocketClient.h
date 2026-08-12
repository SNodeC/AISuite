/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_CLIENT_FRONTENDWEBSOCKETCLIENT_H
#define APPS_CODEX_BACKEND_CLIENT_FRONTENDWEBSOCKETCLIENT_H

#include "ai/openai/codex/frontend/client/Client.h"
#include "core/socket/stream/SocketContextFactory.h"
#include "net/config/ConfigInstance.h"
#include "web/http/ConfigWebSocket.h"
#include "web/http/client/ConfigHTTP.h"
#include "web/http/client/Request.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace core::socket::stream {
    class SocketConnection;
    class SocketContext;
} // namespace core::socket::stream

namespace apps::codex_backend_client {

    struct FrontendWebSocketClientCallbacks {
        std::function<void()> onConnected;
        std::function<void()> onDisconnected;
        std::function<void(std::string)> onFailure;
        std::function<void(bool)> onBeforeTransportConnected;
        std::function<void()> onLocalShutdown = {};
    };

    class FrontendWebSocketClientBinding {
    public:
        FrontendWebSocketClientBinding(ai::openai::codex::frontend::client::Client& client,
                                       FrontendWebSocketClientCallbacks callbacks = {});
        FrontendWebSocketClientBinding(const FrontendWebSocketClientBinding&) = delete;
        FrontendWebSocketClientBinding& operator=(const FrontendWebSocketClientBinding&) = delete;

        void shutdown() noexcept;
        [[nodiscard]] bool connected() const noexcept;
        void reportFailure(std::string message) noexcept;

        // The HTTP context switch invokes its disconnect callback after the
        // successful upgrade response. This single-cycle flag distinguishes
        // that switch from a failed/terminated HTTP connection without
        // manufacturing an application attempt generation.
        void beginUpgrade() noexcept;
        void commitUpgrade() noexcept;
        [[nodiscard]] bool consumeCommittedUpgrade() noexcept;

    private:
        friend class FrontendWebSocketClientSubProtocol;

        ai::openai::codex::frontend::client::Client& client;
        FrontendWebSocketClientCallbacks callbacks;
        class FrontendWebSocketClientSubProtocol* active = nullptr;
        bool upgradeCommitted = false;
    };

    class FrontendWebSocketHttpSocketContextFactory final : public core::socket::stream::SocketContextFactory {
    public:
        using MasterRequest = web::http::client::MasterRequest;

        FrontendWebSocketHttpSocketContextFactory(const std::function<void(const std::shared_ptr<MasterRequest>&)>& onHttpConnected,
                                                  const std::function<void(const std::shared_ptr<MasterRequest>&)>& onHttpDisconnected,
                                                  const std::function<net::config::ConfigInstance&()>& getConfigInstance,
                                                  std::shared_ptr<FrontendWebSocketClientBinding> binding);

    private:
        core::socket::stream::SocketContext* create(core::socket::stream::SocketConnection* socketConnection) override;

        std::function<void(const std::shared_ptr<MasterRequest>&)> onHttpConnected;
        std::function<void(const std::shared_ptr<MasterRequest>&)> onHttpDisconnected;
        net::config::ConfigInstance& configInstance;
        std::shared_ptr<FrontendWebSocketClientBinding> binding;
    };

    template <template <typename SocketContextFactoryT, typename... Args> typename SocketClientT>
    class FrontendWebSocketHttpClient
        : public SocketClientT<FrontendWebSocketHttpSocketContextFactory,
                               std::function<void(const std::shared_ptr<web::http::client::MasterRequest>&)>,
                               std::function<void(const std::shared_ptr<web::http::client::MasterRequest>&)>,
                               std::function<net::config::ConfigInstance&()>,
                               std::shared_ptr<FrontendWebSocketClientBinding>> {
    private:
        using MasterRequest = web::http::client::MasterRequest;
        using Super = SocketClientT<FrontendWebSocketHttpSocketContextFactory,
                                    std::function<void(const std::shared_ptr<MasterRequest>&)>,
                                    std::function<void(const std::shared_ptr<MasterRequest>&)>,
                                    std::function<net::config::ConfigInstance&()>,
                                    std::shared_ptr<FrontendWebSocketClientBinding>>;

    public:
        using SocketConnection = typename Super::SocketConnection;

        FrontendWebSocketHttpClient(const std::string& name,
                                    std::function<void(const std::shared_ptr<MasterRequest>&)>&& onHttpConnected,
                                    std::function<void(const std::shared_ptr<MasterRequest>&)>&& onHttpDisconnected,
                                    std::shared_ptr<FrontendWebSocketClientBinding> binding)
            : Super(
                  name,
                  std::move(onHttpConnected),
                  std::move(onHttpDisconnected),
                  [this]() -> net::config::ConfigInstance& {
                      return *Super::getConfig();
                  },
                  std::move(binding)) {
            Super::getConfig()->net::config::ConfigInstance::template newSubCommand<web::http::client::ConfigHTTP>();
            Super::getConfig()->net::config::ConfigInstance::template newSubCommand<web::http::ConfigWebSocket>();
            Super::setOnConnect(
                [config = Super::getConfig()->net::config::ConfigInstance::template getSubCommand<web::http::client::ConfigHTTP>()](
                    SocketConnection* socketConnection) {
                    if (config->getHostHeader().empty()) {
                        config->setHostHeader(socketConnection->getConfig()->Remote::getSocketAddress().toString(false));
                    }
                });
        }
    };

    void linkFrontendWebSocketClient() noexcept;

} // namespace apps::codex_backend_client

#endif // APPS_CODEX_BACKEND_CLIENT_FRONTENDWEBSOCKETCLIENT_H
