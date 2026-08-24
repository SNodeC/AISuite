/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/bridge/CodexBridge.h"
#include "ai/openai/codex/frontend/StreamSocketContextFactory.h"
#include "apps/codex-bridge/Configuration.h"
#include "apps/codex-bridge/ProviderApplication.h"
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
#include "apps/codex-bridge/WebSocketApplication.h"
#endif
#include "core/SNodeC.h"
#include "core/socket/State.h"
#include "core/timer/Timer.h"
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
#include "express/legacy/in/WebApp.h"
#include "express/legacy/in6/WebApp.h"
#if defined(AISUITE_CODEX_FRONTEND_TLS)
#include "express/tls/in/WebApp.h"
#include "express/tls/in6/WebApp.h"
#endif
#include "web/http/ConfigHttpParser.h"
#include "web/http/ConfigWebSocket.h"
#include "web/http/server/ConfigHttpServer.h"
#endif
#include "net/in/SocketAddress.h"
#include "net/in/stream/legacy/SocketServer.h"
#if defined(AISUITE_CODEX_FRONTEND_TLS)
#include "net/in/stream/tls/SocketServer.h"
#endif
#include "net/in6/SocketAddress.h"
#include "net/in6/stream/legacy/SocketServer.h"
#if defined(AISUITE_CODEX_FRONTEND_TLS)
#include "net/in6/stream/tls/SocketServer.h"
#endif
#include "net/config/ConfigInstance.h"
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
#include "net/rc/stream/legacy/SocketServer.h"
#include "net/rc/stream/tls/SocketServer.h"
#endif
#include "net/un/SocketAddress.h"
#include "net/un/stream/legacy/SocketServer.h"
#include "utils/Config.h"

#include <iostream>
#include <string>

namespace {

    template <typename Address>
    void reportListener(std::string_view name, const Address& address, const core::socket::State& state) {
        if (state == core::socket::State::OK) {
            std::clog << "codex-bridge: " << name << " listener bound at " << address.toString() << '\n';
        } else if (state == core::socket::State::DISABLED) {
            std::clog << "codex-bridge: " << name << " listener disabled\n";
        } else {
            std::cerr << "codex-bridge: " << name << " listener failed: " << state.what() << '\n';
        }
    }

#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
    void configureWebSocketPolicy(net::config::ConfigInstance* config, std::size_t maximumMessageBytes) {
        auto* http = config->getSubCommand<web::http::server::ConfigHttpServer>();
        http->setMaximumPendingRequests(1)->setAllowChunkedTransfer(false)->setAllowPipelining(false);
        http->getParserConfig()
            ->setMaximumStartLineBytes(8192)
            ->setMaximumHeaderLineBytes(8192)
            ->setMaximumHeaderBytes(65536)
            ->setMaximumHeaderFields(128)
            ->setMaximumBodyBytes(1);
        config->getSubCommand<web::http::ConfigWebSocket>()
            ->setMaximumFrameBytes(maximumMessageBytes)
            ->setMaximumMessageBytes(maximumMessageBytes)
            ->setMaximumFragments(4096);
    }
#endif

} // namespace

int main(int argc, char* argv[]) {
    apps::codex_bridge::Configuration* const configuration =
        utils::Config::configRoot.newSubCommand<apps::codex_bridge::Configuration>();
    core::SNodeC::init(argc, argv);

    int result = 1;
    {
        ai::openai::codex::bridge::CodexBridge bridge(configuration->bridgeOptions());
        apps::codex_bridge::ProviderApplication provider(bridge, *configuration);
        const std::size_t maximumFrameBytes = configuration->maximumFrameBytes();

        auto unixServer = net::un::stream::legacy::Server<ai::openai::codex::frontend::StreamSocketContextFactory>(
            "codex-bridge",
            [](net::un::stream::legacy::config::ConfigSocketServer* config) {
                config->Local::setSunPath("/tmp/codex-bridge.sock");
                config->Connection::setReadTimeout(utils::Timeval({0, 0}));
                config->Connection::setWriteTimeout(utils::Timeval({0, 0}));
                config->Connection::setMaximumWriteQueueBytes(apps::codex_bridge::DefaultMaximumWriteQueueBytes);
            },
            bridge,
            maximumFrameBytes);
        auto ipv4Server = net::in::stream::legacy::Server<ai::openai::codex::frontend::StreamSocketContextFactory>(
            "codex-bridge-ipv4",
            [](net::in::stream::legacy::config::ConfigSocketServer* config) {
                config->Instance::setDisabled(true);
                config->Local::setHost("127.0.0.1");
                config->Connection::setReadTimeout(utils::Timeval({0, 0}));
                config->Connection::setWriteTimeout(utils::Timeval({0, 0}));
                config->Connection::setMaximumWriteQueueBytes(apps::codex_bridge::DefaultMaximumWriteQueueBytes);
            },
            bridge,
            maximumFrameBytes);
        auto ipv6Server = net::in6::stream::legacy::Server<ai::openai::codex::frontend::StreamSocketContextFactory>(
            "codex-bridge-ipv6",
            [](net::in6::stream::legacy::config::ConfigSocketServer* config) {
                config->Instance::setDisabled(true);
                config->Local::setHost("::1");
                config->Connection::setReadTimeout(utils::Timeval({0, 0}));
                config->Connection::setWriteTimeout(utils::Timeval({0, 0}));
                config->Connection::setMaximumWriteQueueBytes(apps::codex_bridge::DefaultMaximumWriteQueueBytes);
            },
            bridge,
            maximumFrameBytes);
#if defined(AISUITE_CODEX_FRONTEND_TLS)
        auto tlsIpv4Server = net::in::stream::tls::Server<ai::openai::codex::frontend::StreamSocketContextFactory>(
            "codex-bridge-tls-ipv4",
            [](net::in::stream::tls::config::ConfigSocketServer* config) {
                config->Instance::setDisabled(true);
                config->Local::setHost("127.0.0.1");
                config->Connection::setReadTimeout(utils::Timeval({0, 0}));
                config->Connection::setWriteTimeout(utils::Timeval({0, 0}));
                config->Connection::setMaximumWriteQueueBytes(apps::codex_bridge::DefaultMaximumWriteQueueBytes);
            },
            bridge,
            maximumFrameBytes);
        auto tlsIpv6Server = net::in6::stream::tls::Server<ai::openai::codex::frontend::StreamSocketContextFactory>(
            "codex-bridge-tls-ipv6",
            [](net::in6::stream::tls::config::ConfigSocketServer* config) {
                config->Instance::setDisabled(true);
                config->Local::setHost("::1");
                config->Connection::setReadTimeout(utils::Timeval({0, 0}));
                config->Connection::setWriteTimeout(utils::Timeval({0, 0}));
                config->Connection::setMaximumWriteQueueBytes(apps::codex_bridge::DefaultMaximumWriteQueueBytes);
            },
            bridge,
            maximumFrameBytes);
#endif
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
        auto rfcommServer = net::rc::stream::legacy::Server<ai::openai::codex::frontend::StreamSocketContextFactory>(
            "codex-bridge-rfcomm",
            [](net::rc::stream::legacy::config::ConfigSocketServer* config) {
                config->Instance::setDisabled(true);
                config->Connection::setReadTimeout(utils::Timeval({0, 0}));
                config->Connection::setWriteTimeout(utils::Timeval({0, 0}));
                config->Connection::setMaximumWriteQueueBytes(apps::codex_bridge::DefaultMaximumWriteQueueBytes);
            },
            bridge,
            maximumFrameBytes);
        auto rfcommTlsServer = net::rc::stream::tls::Server<ai::openai::codex::frontend::StreamSocketContextFactory>(
            "codex-bridge-rfcomm-tls",
            [](net::rc::stream::tls::config::ConfigSocketServer* config) {
                config->Instance::setDisabled(true);
                config->Connection::setReadTimeout(utils::Timeval({0, 0}));
                config->Connection::setWriteTimeout(utils::Timeval({0, 0}));
                config->Connection::setMaximumWriteQueueBytes(apps::codex_bridge::DefaultMaximumWriteQueueBytes);
            },
            bridge,
            maximumFrameBytes);
#endif
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        express::legacy::in::WebApp webSocketIpv4App("codex-bridge-websocket-ipv4");
        express::legacy::in6::WebApp webSocketIpv6App("codex-bridge-websocket-ipv6");
        webSocketIpv4App.getConfig()->Instance::setDisabled(true);
        webSocketIpv4App.getConfig()->Local::setHost("127.0.0.1");
        webSocketIpv4App.getConfig()->Connection::setReadTimeout(utils::Timeval({0, 0}));
        webSocketIpv4App.getConfig()->Connection::setWriteTimeout(utils::Timeval({0, 0}));
        webSocketIpv4App.getConfig()->Connection::setMaximumWriteQueueBytes(apps::codex_bridge::DefaultMaximumWriteQueueBytes);
        configureWebSocketPolicy(webSocketIpv4App.getConfig(), maximumFrameBytes);
        webSocketIpv6App.getConfig()->Instance::setDisabled(true);
        webSocketIpv6App.getConfig()->Local::setHost("::1");
        webSocketIpv6App.getConfig()->Connection::setReadTimeout(utils::Timeval({0, 0}));
        webSocketIpv6App.getConfig()->Connection::setWriteTimeout(utils::Timeval({0, 0}));
        webSocketIpv6App.getConfig()->Connection::setMaximumWriteQueueBytes(apps::codex_bridge::DefaultMaximumWriteQueueBytes);
        configureWebSocketPolicy(webSocketIpv6App.getConfig(), maximumFrameBytes);
        apps::codex_bridge::configureWebSocketApplication(
            webSocketIpv4App, bridge, configuration->webSocketEndpoint(), maximumFrameBytes);
        apps::codex_bridge::configureWebSocketApplication(
            webSocketIpv6App, bridge, configuration->webSocketEndpoint(), maximumFrameBytes);
#if defined(AISUITE_CODEX_FRONTEND_TLS)
        express::tls::in::WebApp webSocketTlsIpv4App("codex-bridge-wss-ipv4");
        express::tls::in6::WebApp webSocketTlsIpv6App("codex-bridge-wss-ipv6");
        webSocketTlsIpv4App.getConfig()->Instance::setDisabled(true);
        webSocketTlsIpv4App.getConfig()->Local::setHost("127.0.0.1");
        webSocketTlsIpv4App.getConfig()->Connection::setReadTimeout(utils::Timeval({0, 0}));
        webSocketTlsIpv4App.getConfig()->Connection::setWriteTimeout(utils::Timeval({0, 0}));
        webSocketTlsIpv4App.getConfig()->Connection::setMaximumWriteQueueBytes(apps::codex_bridge::DefaultMaximumWriteQueueBytes);
        configureWebSocketPolicy(webSocketTlsIpv4App.getConfig(), maximumFrameBytes);
        webSocketTlsIpv6App.getConfig()->Instance::setDisabled(true);
        webSocketTlsIpv6App.getConfig()->Local::setHost("::1");
        webSocketTlsIpv6App.getConfig()->Connection::setReadTimeout(utils::Timeval({0, 0}));
        webSocketTlsIpv6App.getConfig()->Connection::setWriteTimeout(utils::Timeval({0, 0}));
        webSocketTlsIpv6App.getConfig()->Connection::setMaximumWriteQueueBytes(apps::codex_bridge::DefaultMaximumWriteQueueBytes);
        configureWebSocketPolicy(webSocketTlsIpv6App.getConfig(), maximumFrameBytes);
        apps::codex_bridge::configureWebSocketApplication(
            webSocketTlsIpv4App, bridge, configuration->webSocketEndpoint(), maximumFrameBytes);
        apps::codex_bridge::configureWebSocketApplication(
            webSocketTlsIpv6App, bridge, configuration->webSocketEndpoint(), maximumFrameBytes);
#endif
#endif

        bool appServerStartupFailed = false;
        static_cast<void>(core::timer::Timer::singleshotTimer(
            [&provider, &appServerStartupFailed] {
                if (!provider.start()) {
                    appServerStartupFailed = true;
                    std::cerr << "codex-bridge: app-server startup failed\n";
                    core::SNodeC::stop();
                }
            },
            utils::Timeval(0)));

        unixServer.listen([](const net::un::SocketAddress& address, const core::socket::State& state) {
            reportListener("unix", address, state);
        });
        ipv4Server.listen([](const net::in::SocketAddress& address, const core::socket::State& state) {
            reportListener("ipv4", address, state);
        });
        ipv6Server.listen([](const net::in6::SocketAddress& address, const core::socket::State& state) {
            reportListener("ipv6", address, state);
        });
#if defined(AISUITE_CODEX_FRONTEND_TLS)
        tlsIpv4Server.listen([](const net::in::SocketAddress& address, const core::socket::State& state) {
            reportListener("tls-ipv4", address, state);
        });
        tlsIpv6Server.listen([](const net::in6::SocketAddress& address, const core::socket::State& state) {
            reportListener("tls-ipv6", address, state);
        });
#endif
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
        rfcommServer.listen([](const net::rc::SocketAddress& address, const core::socket::State& state) {
            reportListener("rfcomm", address, state);
        });
        rfcommTlsServer.listen([](const net::rc::SocketAddress& address, const core::socket::State& state) {
            reportListener("rfcomm-tls", address, state);
        });
#endif
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        webSocketIpv4App.listen([](const net::in::SocketAddress& address, const core::socket::State& state) {
            reportListener("websocket-ipv4", address, state);
        });
        webSocketIpv6App.listen([](const net::in6::SocketAddress& address, const core::socket::State& state) {
            reportListener("websocket-ipv6", address, state);
        });
#if defined(AISUITE_CODEX_FRONTEND_TLS)
        webSocketTlsIpv4App.listen([](const net::in::SocketAddress& address, const core::socket::State& state) {
            reportListener("wss-ipv4", address, state);
        });
        webSocketTlsIpv6App.listen([](const net::in6::SocketAddress& address, const core::socket::State& state) {
            reportListener("wss-ipv6", address, state);
        });
#endif
#endif

        result = core::SNodeC::start();
        provider.stop();
        if (appServerStartupFailed) {
            result = 1;
        }
    }

    return result;
}
