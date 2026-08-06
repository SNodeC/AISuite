/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/client/Client.h"
#include "apps/codex-backend-client/ClientAuthentication.h"
#include "apps/codex-backend-client/ClientConnection.h"
#include "apps/codex-backend-client/CodexBackendClientSocketContextFactory.h"
#include "apps/codex-backend-client/CommandDrainController.h"
#include "apps/codex-backend-client/CommandParser.h"
#include "apps/codex-backend-client/Configuration.h"
#include "apps/codex-backend-client/FrontendWebSocketClient.h"
#include "apps/codex-backend-client/Presenter.h"
#include "apps/codex-backend-client/StdinReader.h"
#include "apps/codex-backend/ReferenceAuthentication.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/socket/State.h"
#include "log/Logger.h"
#include "net/in/stream/legacy/SocketClient.h"
#include "net/in6/stream/legacy/SocketClient.h"
#include "net/un/SocketAddress.h"
#include "net/un/stream/legacy/SocketClient.h"
#if defined(AISUITE_CODEX_FRONTEND_TLS)
#include "net/in/stream/tls/SocketClient.h"
#include "net/in6/stream/tls/SocketClient.h"
#endif
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
#include "net/rc/stream/legacy/SocketClient.h"
#include "net/rc/stream/tls/SocketClient.h"
#endif
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
#include "web/http/legacy/in/Client.h"
#include "web/http/legacy/in6/Client.h"
#if defined(AISUITE_CODEX_FRONTEND_TLS)
#include "web/http/tls/in/Client.h"
#include "web/http/tls/in6/Client.h"
#endif
#endif
#include "utils/Config.h"

#include <array>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>

int main(int argc, char* argv[]) {
    CLI::Option* const bearerTokenFileOption = utils::Config::configRoot.addOption(
        "--bearer-token-file", "Protected bearer-token file for remote frontends", "PATH", std::string{}, CLI::Validator{});
    CLI::Option* const jsonOption = utils::Config::configRoot
                                        .addFlagFunction(
                                            "--json",
                                            []() {
                                                // JSON mode reserves stdout for Frontend Protocol frames.
                                                // Set the ordinary root quiet option as well as the live
                                                // logger because Config parses once during init and again
                                                // when the event loop starts.
                                                utils::Config::configRoot.getOption("--quiet")->add_result("true");
                                                logger::Logger::setQuiet(true);
                                            },
                                            "Emit compact Codex Frontend Protocol JSON on stdout",
                                            "",
                                            CLI::Validator{})
                                        ->trigger_on_parse();

    core::SNodeC::init(argc, argv);

    int result = 1;
    try {
        namespace client = apps::codex_backend_client;
        namespace frontend = ai::openai::codex::frontend;
        namespace sdk_client = ai::openai::codex::frontend::client;

        client::Presenter presenter(jsonOption->as<bool>() ? client::OutputMode::Json : client::OutputMode::Human);
        client::CommandParser parser;
        client::ClientConnection* connectionHandle = nullptr;
        client::FrontendWebSocketClientRuntime* webSocketRuntimeHandle = nullptr;
        client::CommandDrainController* lifecycleHandle = nullptr;
        client::StdinReader* stdinReader = nullptr;
        bool eventLoopRunning = false;
        bool exitScheduled = false;

        std::optional<apps::codex_backend::ProtectedBearerToken> bearerToken;
        const std::string bearerTokenFile = bearerTokenFileOption->as<std::string>();
        if (!bearerTokenFile.empty()) {
            apps::codex_backend::ProtectedTokenFileResult loaded = apps::codex_backend::loadProtectedBearerTokenFile(bearerTokenFile);
            if (const auto* error = std::get_if<apps::codex_backend::ProtectedTokenFileError>(&loaded)) {
                throw std::runtime_error(error->message);
            }
            bearerToken.emplace(std::move(std::get<apps::codex_backend::ProtectedBearerToken>(loaded)));
        }

        client::ClientAuthentication authentication;
        sdk_client::ClientOptions sdkOptions;
        sdkOptions.credentialProvider = [&bearerToken, &authentication] {
            std::optional<frontend::BearerCredential> credential;
            if (bearerToken) {
                credential = bearerToken->credential();
            }
            return authentication.provide(std::move(credential), "verified-local:" + std::to_string(::geteuid()));
        };
        sdk_client::Client sdk(std::move(sdkOptions),
                               sdk_client::ClientCallbacks{.onConnectionStateChanged =
                                                               [&lifecycleHandle](const sdk_client::ConnectionStateChange& change) {
                                                                   if (lifecycleHandle != nullptr) {
                                                                       lifecycleHandle->connectionStateChanged(change.current);
                                                                   }
                                                               },
                                                           .onStateUpdated = {},
                                                           .onSynchronized = {},
                                                           .onCursorAdvanced = {},
                                                           .onProtocolMessage =
                                                               [&presenter](const frontend::ServerMessage& message) {
                                                                   presenter.present(message);
                                                               },
                                                           .onDiagnostic =
                                                               [&presenter](const sdk_client::Diagnostic& diagnostic) {
                                                                   if (diagnostic.severity == sdk_client::Diagnostic::Severity::Error) {
                                                                       presenter.error(diagnostic.message);
                                                                   }
                                                               }});

        client::CommandDrainController lifecycle(
            sdk,
            client::CommandDrainCallbacks{
                .requestExit =
                    [&connectionHandle, &webSocketRuntimeHandle, &stdinReader, &eventLoopRunning, &exitScheduled]() {
                        if (stdinReader != nullptr) {
                            stdinReader->stop();
                        }
                        if (!eventLoopRunning || exitScheduled) {
                            return;
                        }
                        exitScheduled = true;
                        core::EventReceiver::atNextTick([&connectionHandle, &webSocketRuntimeHandle, &eventLoopRunning]() {
                            if (!eventLoopRunning) {
                                return;
                            }
                            if (connectionHandle != nullptr && connectionHandle->connected()) {
                                connectionHandle->disconnect();
                            } else if (webSocketRuntimeHandle != nullptr && webSocketRuntimeHandle->connected()) {
                                webSocketRuntimeHandle->disconnect();
                            } else {
                                core::SNodeC::stop();
                            }
                        });
                    },
                .reportFailure =
                    [&presenter](std::string message) {
                        presenter.error(message);
                    }});
        lifecycleHandle = &lifecycle;

        const auto connectionCallbacks = [&presenter, &lifecycle, &eventLoopRunning, &connectionHandle, &authentication](
                                             std::string transport, client::ClientConnection** owner, bool verifiedLocalUnix) {
            return client::ClientConnectionCallbacks{.onConnected =
                                                         [&presenter, &connectionHandle, owner, transport = std::move(transport)]() {
                                                             connectionHandle = *owner;
                                                             presenter.connected(transport);
                                                             if (presenter.outputMode() == client::OutputMode::Human) {
                                                                 presenter.localMessage("enter 'help' for commands");
                                                             }
                                                         },
                                                     .onDisconnected =
                                                         [&presenter, &lifecycle, &eventLoopRunning, &connectionHandle, owner]() {
                                                             if (connectionHandle != *owner) {
                                                                 return;
                                                             }
                                                             connectionHandle = nullptr;
                                                             presenter.disconnected();
                                                             lifecycle.disconnected();
                                                             if (eventLoopRunning) {
                                                                 core::SNodeC::stop();
                                                             }
                                                         },
                                                     .onFailure =
                                                         [&lifecycle](std::string message) {
                                                             lifecycle.connectionFailed(std::move(message));
                                                         },
                                                     .onOutbound = {},
                                                     .verifiedLocalUnix = verifiedLocalUnix,
                                                     .onBeforeTransportConnected =
                                                         [&authentication](bool localUnix) {
                                                             authentication.prepare(localUnix);
                                                         }};
        };

        client::ClientConnection* unixConnectionHandle = nullptr;
        client::ClientConnection unixConnection(sdk, connectionCallbacks("Unix JSONL", &unixConnectionHandle, true));
        unixConnectionHandle = &unixConnection;
        net::un::stream::legacy::SocketClient<client::CodexBackendClientSocketContextFactory, client::ClientConnection&> unixClient(
            "codex-backend-client-unix", unixConnection);
        unixClient.getConfig()->Remote::setSunPath(client::defaultSocketPath());
        unixClient.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

        client::ClientConnection* ipv4ConnectionHandle = nullptr;
        client::ClientConnection ipv4Connection(sdk, connectionCallbacks("IPv4 JSONL", &ipv4ConnectionHandle, false));
        ipv4ConnectionHandle = &ipv4Connection;
        net::in::stream::legacy::SocketClient<client::CodexBackendClientSocketContextFactory, client::ClientConnection&> ipv4Client(
            "codex-backend-client-ipv4", ipv4Connection);
        ipv4Client.getConfig()->Instance::setDisabled(true);
        ipv4Client.getConfig()->Remote::setHost("127.0.0.1");
        ipv4Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

        client::ClientConnection* ipv6ConnectionHandle = nullptr;
        client::ClientConnection ipv6Connection(sdk, connectionCallbacks("IPv6 JSONL", &ipv6ConnectionHandle, false));
        ipv6ConnectionHandle = &ipv6Connection;
        net::in6::stream::legacy::SocketClient<client::CodexBackendClientSocketContextFactory, client::ClientConnection&> ipv6Client(
            "codex-backend-client-ipv6", ipv6Connection);
        ipv6Client.getConfig()->Instance::setDisabled(true);
        ipv6Client.getConfig()->Remote::setHost("::1");
        ipv6Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

#if defined(AISUITE_CODEX_FRONTEND_TLS)
        client::ClientConnection* tlsIpv4ConnectionHandle = nullptr;
        client::ClientConnection tlsIpv4Connection(sdk, connectionCallbacks("IPv4 TLS JSONL", &tlsIpv4ConnectionHandle, false));
        tlsIpv4ConnectionHandle = &tlsIpv4Connection;
        net::in::stream::tls::SocketClient<client::CodexBackendClientSocketContextFactory, client::ClientConnection&> tlsIpv4Client(
            "codex-backend-client-tls-ipv4", tlsIpv4Connection);
        tlsIpv4Client.getConfig()->Instance::setDisabled(true);
        tlsIpv4Client.getConfig()->Remote::setHost("127.0.0.1");
        tlsIpv4Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

        client::ClientConnection* tlsIpv6ConnectionHandle = nullptr;
        client::ClientConnection tlsIpv6Connection(sdk, connectionCallbacks("IPv6 TLS JSONL", &tlsIpv6ConnectionHandle, false));
        tlsIpv6ConnectionHandle = &tlsIpv6Connection;
        net::in6::stream::tls::SocketClient<client::CodexBackendClientSocketContextFactory, client::ClientConnection&> tlsIpv6Client(
            "codex-backend-client-tls-ipv6", tlsIpv6Connection);
        tlsIpv6Client.getConfig()->Instance::setDisabled(true);
        tlsIpv6Client.getConfig()->Remote::setHost("::1");
        tlsIpv6Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
#endif

#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        client::FrontendWebSocketClientRuntime webSocketRuntime(
            sdk,
            client::FrontendWebSocketClientCallbacks{.onConnected =
                                                         [&presenter]() {
                                                             presenter.connected("WebSocket/WSS");
                                                             if (presenter.outputMode() == client::OutputMode::Human) {
                                                                 presenter.localMessage("enter 'help' for commands");
                                                             }
                                                         },
                                                     .onDisconnected =
                                                         [&presenter, &lifecycle, &eventLoopRunning]() {
                                                             presenter.disconnected();
                                                             lifecycle.disconnected();
                                                             if (eventLoopRunning) {
                                                                 core::SNodeC::stop();
                                                             }
                                                         },
                                                     .onFailure =
                                                         [&lifecycle](std::string message) {
                                                             lifecycle.connectionFailed(std::move(message));
                                                         },
                                                     .onBeforeTransportConnected =
                                                         [&authentication](bool localUnix) {
                                                             authentication.prepare(localUnix);
                                                         }});
        webSocketRuntimeHandle = &webSocketRuntime;
        if (!webSocketRuntime.install()) {
            throw std::runtime_error("failed to install the frontend WebSocket client runtime");
        }
        client::linkFrontendWebSocketClient();

        const auto beginWebSocketUpgrade = [&webSocketRuntime](const auto& request) {
            request->set("Sec-WebSocket-Protocol", "codex");
            request->upgrade(
                "/frontend",
                "websocket",
                [&webSocketRuntime](bool success) {
                    if (!success) {
                        webSocketRuntime.reportFailure("frontend WebSocket upgrade could not be initiated");
                    }
                },
                [&webSocketRuntime](const auto&, const auto& response, bool success) {
                    if (!success || response->get("upgrade") != "websocket" || response->get("sec-websocket-protocol") != "codex") {
                        webSocketRuntime.reportFailure("frontend WebSocket upgrade was rejected");
                    }
                },
                [&webSocketRuntime](const auto&, const std::string& message) {
                    webSocketRuntime.reportFailure("frontend WebSocket HTTP response failed: " + message);
                });
        };
        const auto endWebSocketHttp = [](const auto&) {
        };

        web::http::legacy::in::Client webSocketIpv4Client("codex-backend-client-websocket-ipv4", beginWebSocketUpgrade, endWebSocketHttp);
        webSocketIpv4Client.getConfig()->Instance::setDisabled(true);
        webSocketIpv4Client.getConfig()->Remote::setHost("127.0.0.1");
        webSocketIpv4Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

        web::http::legacy::in6::Client webSocketIpv6Client("codex-backend-client-websocket-ipv6", beginWebSocketUpgrade, endWebSocketHttp);
        webSocketIpv6Client.getConfig()->Instance::setDisabled(true);
        webSocketIpv6Client.getConfig()->Remote::setHost("::1");
        webSocketIpv6Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

#if defined(AISUITE_CODEX_FRONTEND_TLS)
        web::http::tls::in::Client wssIpv4Client("codex-backend-client-wss-ipv4", beginWebSocketUpgrade, endWebSocketHttp);
        wssIpv4Client.getConfig()->Instance::setDisabled(true);
        wssIpv4Client.getConfig()->Remote::setHost("127.0.0.1");
        wssIpv4Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

        web::http::tls::in6::Client wssIpv6Client("codex-backend-client-wss-ipv6", beginWebSocketUpgrade, endWebSocketHttp);
        wssIpv6Client.getConfig()->Instance::setDisabled(true);
        wssIpv6Client.getConfig()->Remote::setHost("::1");
        wssIpv6Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
#endif
#endif

#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
        client::ClientConnection* rfcommConnectionHandle = nullptr;
        client::ClientConnection rfcommConnection(sdk, connectionCallbacks("RFCOMM JSONL", &rfcommConnectionHandle, false));
        rfcommConnectionHandle = &rfcommConnection;
        net::rc::stream::legacy::SocketClient<client::CodexBackendClientSocketContextFactory, client::ClientConnection&> rfcommClient(
            "codex-backend-client-rfcomm", rfcommConnection);
        rfcommClient.getConfig()->Instance::setDisabled(true);
        rfcommClient.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

        client::ClientConnection* rfcommTlsConnectionHandle = nullptr;
        client::ClientConnection rfcommTlsConnection(sdk, connectionCallbacks("RFCOMM TLS JSONL", &rfcommTlsConnectionHandle, false));
        rfcommTlsConnectionHandle = &rfcommTlsConnection;
        net::rc::stream::tls::SocketClient<client::CodexBackendClientSocketContextFactory, client::ClientConnection&> rfcommTlsClient(
            "codex-backend-client-rfcomm-tls", rfcommTlsConnection);
        rfcommTlsClient.getConfig()->Instance::setDisabled(true);
        rfcommTlsClient.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
#endif
        client::StdinReader input(
            [&parser, &presenter, &lifecycle](std::string line) {
                client::ParsedCommand parsed = parser.parse(line);
                std::visit(
                    [&presenter, &lifecycle]<typename Command>(Command&& command) {
                        using T = std::remove_cvref_t<Command>;
                        if constexpr (std::is_same_v<T, client::NoopCommand>) {
                            return;
                        } else if constexpr (std::is_same_v<T, client::HelpCommand>) {
                            presenter.localMessage(client::CommandParser::helpText());
                        } else if constexpr (std::is_same_v<T, client::QuitCommand>) {
                            lifecycle.quit();
                        } else if constexpr (std::is_same_v<T, client::WatchCommand>) {
                            presenter.setWatchEnabled(command.enabled);
                            presenter.localMessage(command.enabled ? "watch on" : "watch off");
                        } else if constexpr (std::is_same_v<T, client::RemoteCommand>) {
                            const bool waitingForInitialSynchronization =
                                lifecycle.sessionState() == client::CommandDrainController::SessionState::Connecting ||
                                lifecycle.sessionState() == client::CommandDrainController::SessionState::Synchronizing;
                            const bool accepted = lifecycle.enqueue(std::move(command));
                            if (!accepted) {
                                if (!lifecycle.failed()) {
                                    presenter.error("command input is closed; command was not queued");
                                }
                            } else if (waitingForInitialSynchronization && presenter.outputMode() == client::OutputMode::Human) {
                                presenter.localMessage("command queued; waiting for initial synchronization");
                            }
                        } else if constexpr (std::is_same_v<T, client::NewCommand>) {
                            const bool waitingForInitialSynchronization =
                                lifecycle.sessionState() == client::CommandDrainController::SessionState::Connecting ||
                                lifecycle.sessionState() == client::CommandDrainController::SessionState::Synchronizing;
                            const bool accepted = lifecycle.enqueue(std::move(command));
                            if (!accepted) {
                                if (!lifecycle.failed()) {
                                    presenter.error("command input is closed; command was not queued");
                                }
                            } else if (waitingForInitialSynchronization && presenter.outputMode() == client::OutputMode::Human) {
                                presenter.localMessage("command queued; waiting for initial synchronization");
                            }
                        } else {
                            presenter.error(command.message);
                        }
                    },
                    std::move(parsed));
            },
            [&lifecycle]() {
                lifecycle.inputEof();
            },
            [&lifecycle](std::string message) {
                lifecycle.inputFailed(std::move(message));
            });
        stdinReader = &input;

        const auto reportConnection = [&lifecycle](std::string transport) {
            return [&lifecycle, transport = std::move(transport)](const auto&, core::socket::State state) {
                if (state != core::socket::State::OK && state != core::socket::State::DISABLED) {
                    lifecycle.connectionFailed("failed to connect using " + transport + ": " + state.what());
                }
            };
        };

        // SNode.C bootstrap applies the final named-instance configuration
        // immediately before the first event-loop turn. Validate those
        // effective values before any physical client starts connecting.
        eventLoopRunning = true;
        core::EventReceiver::atNextTick([&]() {
            if (core::SNodeC::state() != core::State::RUNNING) {
                return;
            }

            const std::array disabledTransports{
                unixClient.getConfig()->Instance::getDisabled(),
                ipv4Client.getConfig()->Instance::getDisabled(),
                ipv6Client.getConfig()->Instance::getDisabled(),
#if defined(AISUITE_CODEX_FRONTEND_TLS)
                tlsIpv4Client.getConfig()->Instance::getDisabled(),
                tlsIpv6Client.getConfig()->Instance::getDisabled(),
#endif
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
                rfcommClient.getConfig()->Instance::getDisabled(),
                rfcommTlsClient.getConfig()->Instance::getDisabled(),
#endif
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
                webSocketIpv4Client.getConfig()->Instance::getDisabled(),
                webSocketIpv6Client.getConfig()->Instance::getDisabled(),
#if defined(AISUITE_CODEX_FRONTEND_TLS)
                wssIpv4Client.getConfig()->Instance::getDisabled(),
                wssIpv6Client.getConfig()->Instance::getDisabled(),
#endif
#endif
            };

            const client::OutgoingTransportPreflight preflight = client::preflightOutgoingTransports(disabledTransports);
            if (!preflight.accepted()) {
                lifecycle.connectionFailed("exactly one outgoing frontend transport must be enabled; found " +
                                           std::to_string(preflight.enabledCount));
                return;
            }

            if (!unixClient.getConfig()->Instance::getDisabled()) {
                unixClient.connect(reportConnection("Unix JSONL"));
            }
            if (!ipv4Client.getConfig()->Instance::getDisabled()) {
                ipv4Client.connect(reportConnection("IPv4 JSONL"));
            }
            if (!ipv6Client.getConfig()->Instance::getDisabled()) {
                ipv6Client.connect(reportConnection("IPv6 JSONL"));
            }
#if defined(AISUITE_CODEX_FRONTEND_TLS)
            if (!tlsIpv4Client.getConfig()->Instance::getDisabled()) {
                tlsIpv4Client.connect(reportConnection("IPv4 TLS JSONL"));
            }
            if (!tlsIpv6Client.getConfig()->Instance::getDisabled()) {
                tlsIpv6Client.connect(reportConnection("IPv6 TLS JSONL"));
            }
#endif
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
            if (!rfcommClient.getConfig()->Instance::getDisabled()) {
                rfcommClient.connect(reportConnection("RFCOMM JSONL"));
            }
            if (!rfcommTlsClient.getConfig()->Instance::getDisabled()) {
                rfcommTlsClient.connect(reportConnection("RFCOMM TLS JSONL"));
            }
#endif
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
            if (!webSocketIpv4Client.getConfig()->Instance::getDisabled()) {
                webSocketIpv4Client.connect(reportConnection("WebSocket IPv4"));
            }
            if (!webSocketIpv6Client.getConfig()->Instance::getDisabled()) {
                webSocketIpv6Client.connect(reportConnection("WebSocket IPv6"));
            }
#if defined(AISUITE_CODEX_FRONTEND_TLS)
            if (!wssIpv4Client.getConfig()->Instance::getDisabled()) {
                wssIpv4Client.connect(reportConnection("WSS IPv4"));
            }
            if (!wssIpv6Client.getConfig()->Instance::getDisabled()) {
                wssIpv6Client.connect(reportConnection("WSS IPv6"));
            }
#endif
#endif
        });

        const int eventLoopResult = core::SNodeC::start();
        eventLoopRunning = false;
        input.stop();
        if (lifecycle.outcome() == client::CommandDrainController::Outcome::Running) {
            lifecycle.quit();
        }
        unixConnection.disconnect();
        ipv4Connection.disconnect();
        ipv6Connection.disconnect();
#if defined(AISUITE_CODEX_FRONTEND_TLS)
        tlsIpv4Connection.disconnect();
        tlsIpv6Connection.disconnect();
#endif
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
        rfcommConnection.disconnect();
        rfcommTlsConnection.disconnect();
#endif
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        webSocketRuntime.uninstall();
        webSocketRuntimeHandle = nullptr;
#endif
        sdk.close();
        result = lifecycle.failed() ? 1 : eventLoopResult;
    } catch (const std::exception& exception) {
        std::cerr << "codex-backend-client: " << exception.what() << '\n';
    } catch (...) {
        std::cerr << "codex-backend-client: unexpected fatal error\n";
    }

    core::SNodeC::free();
    return result;
}
