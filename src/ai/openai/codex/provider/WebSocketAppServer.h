/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_PROVIDER_WEBSOCKETAPPSERVER_H
#define AI_OPENAI_CODEX_PROVIDER_WEBSOCKETAPPSERVER_H

#include "ai/openai/codex/bridge/Endpoint.h"
#include "core/socket/stream/SocketContextFactory.h"
#include "net/config/ConfigInstance.h"
#include "web/http/ConfigWebSocket.h"
#include "web/http/client/ConfigHTTP.h"
#include "web/http/client/Request.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace core::socket::stream {
    class SocketConnection;
    class SocketContext;
}

namespace ai::openai::codex::bridge {
    class CodexBridge;
}

namespace ai::openai::codex::provider {

    class WebSocketAppServerState;

    class WebSocketAppServer final : public bridge::AppServerEndpoint {
    public:
        WebSocketAppServer(bridge::CodexBridge& bridge, std::size_t maximumFrameBytes);
        ~WebSocketAppServer() override;

        WebSocketAppServer(const WebSocketAppServer&) = delete;
        WebSocketAppServer& operator=(const WebSocketAppServer&) = delete;

        bool send(const nlohmann::json& message) override;
        bool isConnected() const noexcept override;
        void stop() noexcept;

        void beginUpgrade(const std::shared_ptr<web::http::client::MasterRequest>& request,
                          std::string endpoint = "/");
        void httpDisconnected(const std::shared_ptr<web::http::client::MasterRequest>& request) noexcept;
        std::shared_ptr<WebSocketAppServerState> state() const noexcept;

    private:
        std::shared_ptr<WebSocketAppServerState> state_;
    };

    class WebSocketHttpSocketContextFactory final : public core::socket::stream::SocketContextFactory {
    public:
        using MasterRequest = web::http::client::MasterRequest;

        WebSocketHttpSocketContextFactory(
            const std::function<void(const std::shared_ptr<MasterRequest>&)>& onHttpConnected,
            const std::function<void(const std::shared_ptr<MasterRequest>&)>& onHttpDisconnected,
            const std::function<net::config::ConfigInstance&()>& getConfigInstance,
            std::shared_ptr<WebSocketAppServerState> state);

        core::socket::stream::SocketContext* create(core::socket::stream::SocketConnection* socketConnection) override;

    private:
        std::function<void(const std::shared_ptr<MasterRequest>&)> onHttpConnected_;
        std::function<void(const std::shared_ptr<MasterRequest>&)> onHttpDisconnected_;
        net::config::ConfigInstance& configInstance_;
        std::shared_ptr<WebSocketAppServerState> state_;
    };

    template <template <typename SocketContextFactoryT, typename... Args> typename SocketClientT>
    class WebSocketHttpClient
        : public SocketClientT<WebSocketHttpSocketContextFactory,
                               std::function<void(const std::shared_ptr<web::http::client::MasterRequest>&)>,
                               std::function<void(const std::shared_ptr<web::http::client::MasterRequest>&)>,
                               std::function<net::config::ConfigInstance&()>,
                               std::shared_ptr<WebSocketAppServerState>> {
    private:
        using MasterRequest = web::http::client::MasterRequest;
        using Super = SocketClientT<WebSocketHttpSocketContextFactory,
                                    std::function<void(const std::shared_ptr<MasterRequest>&)>,
                                    std::function<void(const std::shared_ptr<MasterRequest>&)>,
                                    std::function<net::config::ConfigInstance&()>,
                                    std::shared_ptr<WebSocketAppServerState>>;

    public:
        using SocketConnection = typename Super::SocketConnection;

        WebSocketHttpClient(const std::string& name,
                            std::function<void(const std::shared_ptr<MasterRequest>&)> onHttpConnected,
                            std::function<void(const std::shared_ptr<MasterRequest>&)> onHttpDisconnected,
                            std::shared_ptr<WebSocketAppServerState> state)
            : Super(name,
                    std::move(onHttpConnected),
                    std::move(onHttpDisconnected),
                    [this]() -> net::config::ConfigInstance& { return *Super::getConfig(); },
                    std::move(state)) {
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

    void linkAppServerWebSocketClient();

} // namespace ai::openai::codex::provider

#endif
