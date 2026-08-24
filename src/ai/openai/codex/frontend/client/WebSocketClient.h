/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_WEBSOCKETCLIENT_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_WEBSOCKETCLIENT_H

#include "core/socket/stream/SocketContextFactory.h"
#include "net/config/ConfigInstance.h"
#include "web/http/ConfigWebSocket.h"
#include "web/http/client/ConfigHTTP.h"
#include "web/http/client/Request.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace core::socket::stream {
    class SocketConnection;
    class SocketContext;
}

namespace ai::openai::codex::frontend::client {

    class ClientConnection;
    class WebSocketSubProtocol;

    class WebSocketBinding {
    public:
        WebSocketBinding(ClientConnection& connection, std::size_t maximumFrameBytes);
        ~WebSocketBinding();

        WebSocketBinding(const WebSocketBinding&) = delete;
        WebSocketBinding& operator=(const WebSocketBinding&) = delete;

        void beginUpgrade(const std::shared_ptr<web::http::client::MasterRequest>& request,
                          std::string endpoint = "/codex");
        void httpDisconnected(const std::shared_ptr<web::http::client::MasterRequest>& request) noexcept;
        void shutdown() noexcept;
        bool connected() const noexcept;

    private:
        friend class WebSocketSubProtocol;

        ClientConnection& connection_;
        WebSocketSubProtocol* active_ = nullptr;
        std::size_t maximumFrameBytes_;
        bool upgradeCommitted_ = false;
    };

    class WebSocketHttpSocketContextFactory final : public core::socket::stream::SocketContextFactory {
    public:
        using MasterRequest = web::http::client::MasterRequest;

        WebSocketHttpSocketContextFactory(
            const std::function<void(const std::shared_ptr<MasterRequest>&)>& onHttpConnected,
            const std::function<void(const std::shared_ptr<MasterRequest>&)>& onHttpDisconnected,
            const std::function<net::config::ConfigInstance&()>& getConfigInstance,
            std::shared_ptr<WebSocketBinding> binding);

        core::socket::stream::SocketContext* create(core::socket::stream::SocketConnection* socketConnection) override;

    private:
        std::function<void(const std::shared_ptr<MasterRequest>&)> onHttpConnected_;
        std::function<void(const std::shared_ptr<MasterRequest>&)> onHttpDisconnected_;
        net::config::ConfigInstance& configInstance_;
        std::shared_ptr<WebSocketBinding> binding_;
    };

    template <template <typename SocketContextFactoryT, typename... Args> typename SocketClientT>
    class WebSocketHttpClient
        : public SocketClientT<WebSocketHttpSocketContextFactory,
                               std::function<void(const std::shared_ptr<web::http::client::MasterRequest>&)>,
                               std::function<void(const std::shared_ptr<web::http::client::MasterRequest>&)>,
                               std::function<net::config::ConfigInstance&()>,
                               std::shared_ptr<WebSocketBinding>> {
    private:
        using MasterRequest = web::http::client::MasterRequest;
        using Super = SocketClientT<WebSocketHttpSocketContextFactory,
                                    std::function<void(const std::shared_ptr<MasterRequest>&)>,
                                    std::function<void(const std::shared_ptr<MasterRequest>&)>,
                                    std::function<net::config::ConfigInstance&()>,
                                    std::shared_ptr<WebSocketBinding>>;

    public:
        using SocketConnection = typename Super::SocketConnection;

        WebSocketHttpClient(const std::string& name,
                            std::function<void(const std::shared_ptr<MasterRequest>&)> onHttpConnected,
                            std::function<void(const std::shared_ptr<MasterRequest>&)> onHttpDisconnected,
                            std::shared_ptr<WebSocketBinding> binding)
            : Super(name,
                    std::move(onHttpConnected),
                    std::move(onHttpDisconnected),
                    [this]() -> net::config::ConfigInstance& { return *Super::getConfig(); },
                    std::move(binding)) {
            Super::getConfig()->net::config::ConfigInstance::template newSubCommand<web::http::client::ConfigHTTP>();
            Super::getConfig()->net::config::ConfigInstance::template newSubCommand<web::http::ConfigWebSocket>();
            Super::setOnConnect(
                [config = Super::getConfig()->net::config::ConfigInstance::template getSubCommand<web::http::client::ConfigHTTP>()](
                    SocketConnection* connection) {
                    if (config->getHostHeader().empty()) {
                        config->setHostHeader(connection->getConfig()->Remote::getSocketAddress().toString(false));
                    }
                });
        }
    };

    void linkWebSocketClient();

} // namespace ai::openai::codex::frontend::client

#endif
