/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex2/frontend/CodexBridge.h"
#include "ai/openai/codex2/frontend/client/ClientConnection.h"
#include "ai/openai/codex2/frontend/client/StreamSocketContextFactory.h"
#if defined(AISUITE_CODEX2_FRONTEND_WEBSOCKET)
#include "ai/openai/codex2/frontend/client/WebSocketClient.h"
#endif
#include "apps/codex-bridge-client/ClientSession.h"
#include "apps/codex-bridge-client/CommandParser.h"
#include "apps/codex-bridge-client/Configuration.h"
#include "apps/codex-bridge-client/Presenter.h"
#include "apps/codex-bridge-client/StdinReader.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/socket/State.h"
#include "net/config/ConfigInstance.h"
#include "net/in/stream/legacy/SocketClient.h"
#include "net/in6/stream/legacy/SocketClient.h"
#include "net/un/stream/legacy/SocketClient.h"
#if defined(AISUITE_CODEX2_FRONTEND_TLS)
#include "net/in/stream/tls/SocketClient.h"
#include "net/in6/stream/tls/SocketClient.h"
#endif
#if defined(AISUITE_CODEX2_FRONTEND_RFCOMM)
#include "net/rc/stream/legacy/SocketClient.h"
#include "net/rc/stream/tls/SocketClient.h"
#endif
#if defined(AISUITE_CODEX2_FRONTEND_WEBSOCKET)
#include "web/http/client/Request.h"
#endif
#include "utils/Config.h"

#include <array>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

    template <typename Client>
    void configureStreamClient(Client& client, bool disabled) {
        client.getConfig()->Instance::setDisabled(disabled);
        client.getConfig()->Connection::setReadTimeout(utils::Timeval({0, 0}));
        client.getConfig()->Connection::setWriteTimeout(utils::Timeval({0, 0}));
        client.getConfig()->Connection::setMaximumWriteQueueBytes(
            apps::codex_bridge_client::DefaultMaximumWriteQueueBytes);
    }

} // namespace

int main(int argc, char* argv[]) {
    namespace app = apps::codex_bridge_client;
    namespace frontend = ai::openai::codex2::frontend;
    namespace client = ai::openai::codex2::frontend::client;

    app::Configuration* const configuration =
        utils::Config::configRoot.newSubCommand<app::Configuration>();
    core::SNodeC::init(argc, argv);

    int result = 1;
    try {
        app::Presenter presenter(configuration->jsonOutput() ? app::OutputMode::Json : app::OutputMode::Human);
        app::CommandParser parser;
        frontend::CodexBridge sdk({});

        std::string activeTransport;
        std::function<void()> connectSelected;
        std::function<void()> terminateSelected;
        std::function<bool()> selectedFlowTerminated;
        std::function<void()> requestReconnect;
        std::function<void()> requestQuit;
        std::function<void()> continueReconnect;
        app::StdinReader* input = nullptr;
        bool eventLoopRunning = false;
        bool reconnectPending = false;
        bool shutdownRequested = false;

        client::ClientConnection connection(
            sdk,
            client::ClientConnectionCallbacks{
                .onConnected = [&] {
                    presenter.connected(activeTransport);
                    if (presenter.outputMode() == app::OutputMode::Human) {
                        presenter.localMessage("enter 'help' for commands");
                    }
                },
                .onDisconnected = [&] { presenter.disconnected(); },
                .onFailure = [&](std::string reason) { presenter.error(reason); }});

        app::ClientSession session(
            sdk,
            presenter,
            [&] {
                if (requestReconnect) {
                    requestReconnect();
                } else {
                    presenter.error("no frontend transport has been selected");
                }
            },
            [&] {
                if (requestQuit) {
                    requestQuit();
                }
            });

        const std::size_t maximumFrameBytes = configuration->maximumFrameBytes();
        using StreamFactory = client::StreamSocketContextFactory;

        net::un::stream::legacy::SocketClient<StreamFactory, client::ClientConnection&, std::size_t> unixClient(
            "codex-bridge-client-unix", connection, std::size_t(maximumFrameBytes));
        unixClient.getConfig()->Remote::setSunPath("/tmp/codex-bridge.sock");
        unixClient.getConfig()->Connection::setReadTimeout(utils::Timeval({0, 0}));
        unixClient.getConfig()->Connection::setWriteTimeout(utils::Timeval({0, 0}));
        unixClient.getConfig()->Connection::setMaximumWriteQueueBytes(app::DefaultMaximumWriteQueueBytes);

        net::in::stream::legacy::SocketClient<StreamFactory, client::ClientConnection&, std::size_t> ipv4Client(
            "codex-bridge-client-ipv4", connection, std::size_t(maximumFrameBytes));
        configureStreamClient(ipv4Client, true);
        ipv4Client.getConfig()->Remote::setHost("127.0.0.1");

        net::in6::stream::legacy::SocketClient<StreamFactory, client::ClientConnection&, std::size_t> ipv6Client(
            "codex-bridge-client-ipv6", connection, std::size_t(maximumFrameBytes));
        configureStreamClient(ipv6Client, true);
        ipv6Client.getConfig()->Remote::setHost("::1");

#if defined(AISUITE_CODEX2_FRONTEND_TLS)
        net::in::stream::tls::SocketClient<StreamFactory, client::ClientConnection&, std::size_t> tlsIpv4Client(
            "codex-bridge-client-tls-ipv4", connection, std::size_t(maximumFrameBytes));
        configureStreamClient(tlsIpv4Client, true);
        tlsIpv4Client.getConfig()->Remote::setHost("127.0.0.1");

        net::in6::stream::tls::SocketClient<StreamFactory, client::ClientConnection&, std::size_t> tlsIpv6Client(
            "codex-bridge-client-tls-ipv6", connection, std::size_t(maximumFrameBytes));
        configureStreamClient(tlsIpv6Client, true);
        tlsIpv6Client.getConfig()->Remote::setHost("::1");
#endif

#if defined(AISUITE_CODEX2_FRONTEND_RFCOMM)
        net::rc::stream::legacy::SocketClient<StreamFactory, client::ClientConnection&, std::size_t> rfcommClient(
            "codex-bridge-client-rfcomm", connection, std::size_t(maximumFrameBytes));
        configureStreamClient(rfcommClient, true);

        net::rc::stream::tls::SocketClient<StreamFactory, client::ClientConnection&, std::size_t> rfcommTlsClient(
            "codex-bridge-client-rfcomm-tls", connection, std::size_t(maximumFrameBytes));
        configureStreamClient(rfcommTlsClient, true);
#endif

#if defined(AISUITE_CODEX2_FRONTEND_WEBSOCKET)
        client::linkWebSocketClient();
        auto webSocketBinding = std::make_shared<client::WebSocketBinding>(connection, maximumFrameBytes);
        const auto beginWebSocket = [webSocketBinding, configuration](
                                        const std::shared_ptr<web::http::client::MasterRequest>& request) {
            webSocketBinding->beginUpgrade(request, configuration->webSocketEndpoint());
        };
        const auto endWebSocket = [webSocketBinding](
                                      const std::shared_ptr<web::http::client::MasterRequest>& request) {
            webSocketBinding->httpDisconnected(request);
        };

        client::WebSocketHttpClient<net::in::stream::legacy::SocketClient> webSocketIpv4Client(
            "codex-bridge-client-websocket-ipv4", beginWebSocket, endWebSocket, webSocketBinding);
        configureStreamClient(webSocketIpv4Client, true);
        webSocketIpv4Client.getConfig()->Remote::setHost("127.0.0.1");

        client::WebSocketHttpClient<net::in6::stream::legacy::SocketClient> webSocketIpv6Client(
            "codex-bridge-client-websocket-ipv6", beginWebSocket, endWebSocket, webSocketBinding);
        configureStreamClient(webSocketIpv6Client, true);
        webSocketIpv6Client.getConfig()->Remote::setHost("::1");

#if defined(AISUITE_CODEX2_FRONTEND_TLS)
        client::WebSocketHttpClient<net::in::stream::tls::SocketClient> wssIpv4Client(
            "codex-bridge-client-wss-ipv4", beginWebSocket, endWebSocket, webSocketBinding);
        configureStreamClient(wssIpv4Client, true);
        wssIpv4Client.getConfig()->Remote::setHost("127.0.0.1");

        client::WebSocketHttpClient<net::in6::stream::tls::SocketClient> wssIpv6Client(
            "codex-bridge-client-wss-ipv6", beginWebSocket, endWebSocket, webSocketBinding);
        configureStreamClient(wssIpv6Client, true);
        wssIpv6Client.getConfig()->Remote::setHost("::1");
#endif
#endif

        const auto selectClient = [&](auto& configuredClient, std::string transport) {
            auto* const clientHandle = &configuredClient;
            auto* const flow = configuredClient.getFlowController();
            activeTransport = std::move(transport);
            connectSelected = [&, clientHandle, flow] {
                clientHandle->connect([&, flow](const auto&, core::socket::State state) {
                    if (state == core::socket::State::OK || state == core::socket::State::DISABLED) {
                        return;
                    }
                    const std::string failure = "failed to connect using " + activeTransport + ": " + state.what();
                    core::EventReceiver::atNextTick([&, flow, failure] {
                        if (eventLoopRunning && !shutdownRequested && flow->isTerminated()) {
                            presenter.error(failure);
                        }
                    });
                });
            };
            terminateSelected = [flow] { static_cast<void>(flow->terminateFlow()); };
            selectedFlowTerminated = [flow] { return flow->isTerminated(); };
        };

        continueReconnect = [&] {
            if (!eventLoopRunning || shutdownRequested || !reconnectPending) {
                return;
            }
            if (!selectedFlowTerminated || !selectedFlowTerminated() || connection.attached()) {
                core::EventReceiver::atNextTick(continueReconnect);
                return;
            }
            reconnectPending = false;
            connectSelected();
        };
        requestReconnect = [&] {
            if (!eventLoopRunning || shutdownRequested || !connectSelected || !terminateSelected) {
                presenter.error("configured frontend transport is unavailable");
                return;
            }
            if (reconnectPending) {
                presenter.error("frontend reconnect is already in progress");
                return;
            }
            reconnectPending = true;
            connection.disconnect("explicit frontend reconnect");
            terminateSelected();
            core::EventReceiver::atNextTick(continueReconnect);
        };
        requestQuit = [&] {
            if (shutdownRequested) {
                return;
            }
            shutdownRequested = true;
            reconnectPending = false;
            if (input != nullptr) {
                input->stop();
            }
            connection.shutdown();
            if (terminateSelected) {
                terminateSelected();
            }
            if (eventLoopRunning) {
                core::SNodeC::stop();
            }
        };

        app::StdinReader stdinReader(
            maximumFrameBytes,
            [&](std::string line) { session.execute(parser.parse(line)); },
            [&] { requestQuit(); },
            [&](std::string reason) {
                presenter.error(reason);
                requestQuit();
            });
        input = &stdinReader;

        eventLoopRunning = true;
        core::EventReceiver::atNextTick([&] {
            const std::array disabled{
                unixClient.getConfig()->Instance::getDisabled(),
                ipv4Client.getConfig()->Instance::getDisabled(),
                ipv6Client.getConfig()->Instance::getDisabled(),
#if defined(AISUITE_CODEX2_FRONTEND_TLS)
                tlsIpv4Client.getConfig()->Instance::getDisabled(),
                tlsIpv6Client.getConfig()->Instance::getDisabled(),
#endif
#if defined(AISUITE_CODEX2_FRONTEND_RFCOMM)
                rfcommClient.getConfig()->Instance::getDisabled(),
                rfcommTlsClient.getConfig()->Instance::getDisabled(),
#endif
#if defined(AISUITE_CODEX2_FRONTEND_WEBSOCKET)
                webSocketIpv4Client.getConfig()->Instance::getDisabled(),
                webSocketIpv6Client.getConfig()->Instance::getDisabled(),
#if defined(AISUITE_CODEX2_FRONTEND_TLS)
                wssIpv4Client.getConfig()->Instance::getDisabled(),
                wssIpv6Client.getConfig()->Instance::getDisabled(),
#endif
#endif
            };
            std::size_t enabledCount = 0;
            for (const bool isDisabled : disabled) {
                enabledCount += isDisabled ? 0U : 1U;
            }
            if (enabledCount != 1) {
                presenter.error("exactly one outgoing bridge transport must be enabled; found " +
                                std::to_string(enabledCount));
                requestQuit();
                return;
            }

            if (!unixClient.getConfig()->Instance::getDisabled()) {
                selectClient(unixClient, "Unix JSONL");
            } else if (!ipv4Client.getConfig()->Instance::getDisabled()) {
                selectClient(ipv4Client, "IPv4 JSONL");
            } else if (!ipv6Client.getConfig()->Instance::getDisabled()) {
                selectClient(ipv6Client, "IPv6 JSONL");
            }
#if defined(AISUITE_CODEX2_FRONTEND_TLS)
            else if (!tlsIpv4Client.getConfig()->Instance::getDisabled()) {
                selectClient(tlsIpv4Client, "IPv4 TLS JSONL");
            } else if (!tlsIpv6Client.getConfig()->Instance::getDisabled()) {
                selectClient(tlsIpv6Client, "IPv6 TLS JSONL");
            }
#endif
#if defined(AISUITE_CODEX2_FRONTEND_RFCOMM)
            else if (!rfcommClient.getConfig()->Instance::getDisabled()) {
                selectClient(rfcommClient, "RFCOMM JSONL");
            } else if (!rfcommTlsClient.getConfig()->Instance::getDisabled()) {
                selectClient(rfcommTlsClient, "RFCOMM TLS JSONL");
            }
#endif
#if defined(AISUITE_CODEX2_FRONTEND_WEBSOCKET)
            else if (!webSocketIpv4Client.getConfig()->Instance::getDisabled()) {
                selectClient(webSocketIpv4Client, "WebSocket IPv4");
            } else if (!webSocketIpv6Client.getConfig()->Instance::getDisabled()) {
                selectClient(webSocketIpv6Client, "WebSocket IPv6");
            }
#if defined(AISUITE_CODEX2_FRONTEND_TLS)
            else if (!wssIpv4Client.getConfig()->Instance::getDisabled()) {
                selectClient(wssIpv4Client, "WSS IPv4");
            } else if (!wssIpv6Client.getConfig()->Instance::getDisabled()) {
                selectClient(wssIpv6Client, "WSS IPv6");
            }
#endif
#endif
            connectSelected();
        });

        result = core::SNodeC::start();
        eventLoopRunning = false;
        stdinReader.stop();
        reconnectPending = false;
#if defined(AISUITE_CODEX2_FRONTEND_WEBSOCKET)
        webSocketBinding->shutdown();
#endif
        if (terminateSelected) {
            terminateSelected();
        }
        connection.shutdown();
    } catch (const std::exception& exception) {
        std::cerr << "codex-bridge-client: " << exception.what() << '\n';
    } catch (...) {
        std::cerr << "codex-bridge-client: unexpected fatal error\n";
    }

    return result;
}
