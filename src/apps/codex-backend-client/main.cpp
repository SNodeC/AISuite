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
#include "net/config/ConfigInstance.h"
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
#include "web/http/client/Request.h"
#include "web/http/client/SocketContext.h"
#endif
#include "utils/Config.h"

#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>

namespace {

#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
    bool closeWebSocketUpgradeTransport(const std::shared_ptr<web::http::client::MasterRequest>& request) noexcept {
        try {
            // MasterRequest::disconnect() only detaches its pipe sink and
            // clears its SocketContext pointer. Closing the context is what
            // actually cancels the pre-upgrade physical connection.
            if (request != nullptr && request->getSocketContext() != nullptr) {
                request->getSocketContext()->close();
                return true;
            }
        } catch (...) {
        }
        return false;
    }

#endif

} // namespace

int main(int argc, char* argv[]) {
    apps::codex_backend_client::ClientPolicyConfiguration clientPolicyConfiguration;
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
        client::FrontendWebSocketClientBinding* webSocketBindingHandle = nullptr;
        client::CommandDrainController* lifecycleHandle = nullptr;
        client::StdinReader* stdinReader = nullptr;
        std::function<void()> stopConfiguredClientFlow;
        std::function<std::optional<std::string>()> prepareExplicitReconnect;
        std::function<void()> beginConnectionAttempt;
        bool disconnectedPresented = false;
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
                                                                       lifecycleHandle->connectionStateChanged(change);
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

        const auto shutdownActiveTransport = [&]() {
            if (connectionHandle != nullptr && connectionHandle->connected()) {
                connectionHandle->shutdown();
                return;
            }
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
            if (webSocketBindingHandle != nullptr && webSocketBindingHandle->connected()) {
                webSocketBindingHandle->shutdown();
                return;
            }
#endif
            if (stopConfiguredClientFlow) {
                stopConfiguredClientFlow();
            }
            core::SNodeC::stop();
        };

        client::CommandDrainController lifecycle(
            sdk,
            client::CommandDrainCallbacks{
                .requestExit =
                    [&stdinReader, &eventLoopRunning, &exitScheduled, &shutdownActiveTransport]() {
                        if (stdinReader != nullptr) {
                            stdinReader->stop();
                        }
                        if (!eventLoopRunning || exitScheduled) {
                            return;
                        }
                        exitScheduled = true;
                        core::EventReceiver::atNextTick([&eventLoopRunning, &shutdownActiveTransport]() {
                            if (!eventLoopRunning) {
                                return;
                            }
                            shutdownActiveTransport();
                        });
                    },
                .reportFailure =
                    [&presenter](std::string message) {
                        presenter.error(message);
                    },
                .requestReconnect =
                    [&beginConnectionAttempt,
                     &prepareExplicitReconnect,
                     &eventLoopRunning,
                     &disconnectedPresented,
                     &lifecycleHandle,
                     &presenter]() {
                        if (!eventLoopRunning || !beginConnectionAttempt) {
                            return std::optional<std::string>{"configured frontend transport is unavailable"};
                        }
                        if (prepareExplicitReconnect) {
                            if (std::optional<std::string> rejection = prepareExplicitReconnect()) {
                                return rejection;
                            }
                        }
                        disconnectedPresented = false;
                        core::EventReceiver::atNextTick(
                            [&beginConnectionAttempt, &eventLoopRunning, &lifecycleHandle, &presenter, &disconnectedPresented]() {
                                if (eventLoopRunning && beginConnectionAttempt && lifecycleHandle != nullptr &&
                                    !lifecycleHandle->applicationShutdownActive() &&
                                    lifecycleHandle->sessionState() == client::CommandDrainController::SessionState::Connecting) {
                                    try {
                                        beginConnectionAttempt();
                                    } catch (...) {
                                        if (lifecycleHandle != nullptr) {
                                            lifecycleHandle->connectionAttemptFailed("failed to create the configured frontend transport");
                                        }
                                        if (!disconnectedPresented) {
                                            presenter.disconnected();
                                            disconnectedPresented = true;
                                        }
                                    }
                                }
                            });
                        return std::optional<std::string>{};
                    }},
            clientPolicyConfiguration.commandQueueLimits());
        lifecycleHandle = &lifecycle;

        const auto nativeConnectionCallbacks =
            [&presenter,
             &lifecycle,
             &eventLoopRunning,
             &connectionHandle,
             &authentication,
             &stopConfiguredClientFlow,
             &disconnectedPresented](std::string transport, client::ClientConnection** owner, bool verifiedLocalUnix) {
                return client::ClientConnectionCallbacks{
                    .onConnected =
                        [&presenter, &connectionHandle, &disconnectedPresented, owner, transport = std::move(transport)]() {
                            connectionHandle = *owner;
                            disconnectedPresented = false;
                            presenter.connected(transport);
                            if (presenter.outputMode() == client::OutputMode::Human) {
                                presenter.localMessage("enter 'help' for commands");
                            }
                        },
                    .onDisconnected =
                        [&presenter, &lifecycle, &eventLoopRunning, &connectionHandle, &disconnectedPresented, owner]() {
                            if (connectionHandle == *owner) {
                                connectionHandle = nullptr;
                            }
                            if (!disconnectedPresented) {
                                presenter.disconnected();
                                disconnectedPresented = true;
                            }
                            lifecycle.disconnected();
                            if (eventLoopRunning && lifecycle.applicationShutdownActive()) {
                                core::SNodeC::stop();
                            }
                        },
                    .onFailure =
                        [&lifecycle](std::string message) {
                            lifecycle.connectionFailed(std::move(message));
                        },
                    .onAttemptConnected = {},
                    .onAttemptDisconnected = {},
                    .onAttemptFailure = {},
                    .onOutbound = {},
                    .verifiedLocalUnix = verifiedLocalUnix,
                    .onBeforeTransportConnected =
                        [&authentication](bool localUnix) {
                            authentication.prepare(localUnix);
                        },
                    .onLocalShutdown =
                        [&lifecycle, &stopConfiguredClientFlow]() {
                            if (stopConfiguredClientFlow) {
                                stopConfiguredClientFlow();
                            }
                            lifecycle.localShutdownRequested();
                        }};
            };

        client::ClientConnection* unixConnectionHandle = nullptr;
        client::ClientConnection unixConnection(sdk, nativeConnectionCallbacks("Unix JSONL", &unixConnectionHandle, true));
        unixConnectionHandle = &unixConnection;
        net::un::stream::legacy::SocketClient<client::CodexBackendClientSocketContextFactory, client::ClientConnection&> unixClient(
            "codex-backend-client-unix", unixConnection);
        unixClient.getConfig()->Remote::setSunPath(client::defaultSocketPath());
        unixClient.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

        client::ClientConnection* ipv4ConnectionHandle = nullptr;
        client::ClientConnection ipv4Connection(sdk, nativeConnectionCallbacks("IPv4 JSONL", &ipv4ConnectionHandle, false));
        ipv4ConnectionHandle = &ipv4Connection;
        net::in::stream::legacy::SocketClient<client::CodexBackendClientSocketContextFactory, client::ClientConnection&> ipv4Client(
            "codex-backend-client-ipv4", ipv4Connection);
        ipv4Client.getConfig()->Instance::setDisabled(true);
        ipv4Client.getConfig()->Remote::setHost("127.0.0.1");
        ipv4Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

        client::ClientConnection* ipv6ConnectionHandle = nullptr;
        client::ClientConnection ipv6Connection(sdk, nativeConnectionCallbacks("IPv6 JSONL", &ipv6ConnectionHandle, false));
        ipv6ConnectionHandle = &ipv6Connection;
        net::in6::stream::legacy::SocketClient<client::CodexBackendClientSocketContextFactory, client::ClientConnection&> ipv6Client(
            "codex-backend-client-ipv6", ipv6Connection);
        ipv6Client.getConfig()->Instance::setDisabled(true);
        ipv6Client.getConfig()->Remote::setHost("::1");
        ipv6Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

#if defined(AISUITE_CODEX_FRONTEND_TLS)
        client::ClientConnection* tlsIpv4ConnectionHandle = nullptr;
        client::ClientConnection tlsIpv4Connection(sdk, nativeConnectionCallbacks("IPv4 TLS JSONL", &tlsIpv4ConnectionHandle, false));
        tlsIpv4ConnectionHandle = &tlsIpv4Connection;
        net::in::stream::tls::SocketClient<client::CodexBackendClientSocketContextFactory, client::ClientConnection&> tlsIpv4Client(
            "codex-backend-client-tls-ipv4", tlsIpv4Connection);
        tlsIpv4Client.getConfig()->Instance::setDisabled(true);
        tlsIpv4Client.getConfig()->Remote::setHost("127.0.0.1");
        tlsIpv4Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

        client::ClientConnection* tlsIpv6ConnectionHandle = nullptr;
        client::ClientConnection tlsIpv6Connection(sdk, nativeConnectionCallbacks("IPv6 TLS JSONL", &tlsIpv6ConnectionHandle, false));
        tlsIpv6ConnectionHandle = &tlsIpv6Connection;
        net::in6::stream::tls::SocketClient<client::CodexBackendClientSocketContextFactory, client::ClientConnection&> tlsIpv6Client(
            "codex-backend-client-tls-ipv6", tlsIpv6Connection);
        tlsIpv6Client.getConfig()->Instance::setDisabled(true);
        tlsIpv6Client.getConfig()->Remote::setHost("::1");
        tlsIpv6Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
#endif

#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        auto webSocketBinding = std::make_shared<client::FrontendWebSocketClientBinding>(
            sdk,
            client::FrontendWebSocketClientCallbacks{.onConnected =
                                                         [&presenter, &disconnectedPresented]() {
                                                             disconnectedPresented = false;
                                                             presenter.connected("WebSocket/WSS");
                                                             if (presenter.outputMode() == client::OutputMode::Human) {
                                                                 presenter.localMessage("enter 'help' for commands");
                                                             }
                                                         },
                                                     .onDisconnected =
                                                         [&presenter, &lifecycle, &eventLoopRunning, &disconnectedPresented]() {
                                                             if (!disconnectedPresented) {
                                                                 presenter.disconnected();
                                                                 disconnectedPresented = true;
                                                             }
                                                             lifecycle.disconnected();
                                                             if (eventLoopRunning && lifecycle.applicationShutdownActive()) {
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
                                                         },
                                                     .onLocalShutdown =
                                                         [&lifecycle]() {
                                                             lifecycle.localShutdownRequested();
                                                         }});
        webSocketBindingHandle = webSocketBinding.get();
        client::linkFrontendWebSocketClient();

        const auto beginWebSocketUpgrade = [webSocketBinding](const std::shared_ptr<web::http::client::MasterRequest>& request) {
            webSocketBinding->beginUpgrade();
            const std::weak_ptr<web::http::client::MasterRequest> requestWeak = request;
            const auto failUpgrade = [webSocketBinding, requestWeak](std::string message) {
                webSocketBinding->reportFailure(std::move(message));
                if (const std::shared_ptr<web::http::client::MasterRequest> activeRequest = requestWeak.lock()) {
                    static_cast<void>(closeWebSocketUpgradeTransport(activeRequest));
                }
            };
            request->set("Sec-WebSocket-Protocol", "codex");
            request->upgrade(
                "/frontend",
                "websocket",
                [failUpgrade](bool success) {
                    if (!success) {
                        failUpgrade("frontend WebSocket upgrade could not be initiated");
                    }
                },
                [webSocketBinding, failUpgrade](const auto&, const auto& response, bool success) {
                    if (!success || response->get("upgrade") != "websocket" || response->get("sec-websocket-protocol") != "codex") {
                        failUpgrade("frontend WebSocket upgrade was rejected");
                        return;
                    }
                    webSocketBinding->commitUpgrade();
                },
                [failUpgrade](const auto&, const std::string& message) {
                    failUpgrade("frontend WebSocket HTTP response failed: " + message);
                });
        };
        const auto endWebSocketHttp = [&presenter, &lifecycle, &eventLoopRunning, &disconnectedPresented, webSocketBinding](
                                          const std::shared_ptr<web::http::client::MasterRequest>&) {
            if (webSocketBinding->consumeCommittedUpgrade() || webSocketBinding->connected()) {
                return;
            }
            if (!disconnectedPresented) {
                presenter.disconnected();
                disconnectedPresented = true;
            }
            lifecycle.disconnected();
            if (eventLoopRunning && lifecycle.applicationShutdownActive()) {
                core::SNodeC::stop();
            }
        };

        client::FrontendWebSocketHttpClient<net::in::stream::legacy::SocketClient> webSocketIpv4Client(
            "codex-backend-client-websocket-ipv4", beginWebSocketUpgrade, endWebSocketHttp, webSocketBinding);
        webSocketIpv4Client.getConfig()->Instance::setDisabled(true);
        webSocketIpv4Client.getConfig()->Remote::setHost("127.0.0.1");
        webSocketIpv4Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

        client::FrontendWebSocketHttpClient<net::in6::stream::legacy::SocketClient> webSocketIpv6Client(
            "codex-backend-client-websocket-ipv6", beginWebSocketUpgrade, endWebSocketHttp, webSocketBinding);
        webSocketIpv6Client.getConfig()->Instance::setDisabled(true);
        webSocketIpv6Client.getConfig()->Remote::setHost("::1");
        webSocketIpv6Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

#if defined(AISUITE_CODEX_FRONTEND_TLS)
        client::FrontendWebSocketHttpClient<net::in::stream::tls::SocketClient> wssIpv4Client(
            "codex-backend-client-wss-ipv4", beginWebSocketUpgrade, endWebSocketHttp, webSocketBinding);
        wssIpv4Client.getConfig()->Instance::setDisabled(true);
        wssIpv4Client.getConfig()->Remote::setHost("127.0.0.1");
        wssIpv4Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

        client::FrontendWebSocketHttpClient<net::in6::stream::tls::SocketClient> wssIpv6Client(
            "codex-backend-client-wss-ipv6", beginWebSocketUpgrade, endWebSocketHttp, webSocketBinding);
        wssIpv6Client.getConfig()->Instance::setDisabled(true);
        wssIpv6Client.getConfig()->Remote::setHost("::1");
        wssIpv6Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
#endif
#endif

#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
        client::ClientConnection* rfcommConnectionHandle = nullptr;
        client::ClientConnection rfcommConnection(sdk, nativeConnectionCallbacks("RFCOMM JSONL", &rfcommConnectionHandle, false));
        rfcommConnectionHandle = &rfcommConnection;
        net::rc::stream::legacy::SocketClient<client::CodexBackendClientSocketContextFactory, client::ClientConnection&> rfcommClient(
            "codex-backend-client-rfcomm", rfcommConnection);
        rfcommClient.getConfig()->Instance::setDisabled(true);
        rfcommClient.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

        client::ClientConnection* rfcommTlsConnectionHandle = nullptr;
        client::ClientConnection rfcommTlsConnection(sdk, nativeConnectionCallbacks("RFCOMM TLS JSONL", &rfcommTlsConnectionHandle, false));
        rfcommTlsConnectionHandle = &rfcommTlsConnection;
        net::rc::stream::tls::SocketClient<client::CodexBackendClientSocketContextFactory, client::ClientConnection&> rfcommTlsClient(
            "codex-backend-client-rfcomm-tls", rfcommTlsConnection);
        rfcommTlsClient.getConfig()->Instance::setDisabled(true);
        rfcommTlsClient.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
#endif
        client::StdinReader input(
            [&parser, &presenter, &lifecycle](std::string line) {
                const std::size_t retainedInputBytes = line.size();
                client::ParsedCommand parsed = parser.parse(line);
                std::visit(
                    [&presenter, &lifecycle, retainedInputBytes]<typename Command>(Command&& command) {
                        using T = std::remove_cvref_t<Command>;
                        if constexpr (std::is_same_v<T, client::NoopCommand>) {
                            return;
                        } else if constexpr (std::is_same_v<T, client::HelpCommand>) {
                            presenter.localMessage(client::CommandParser::helpText());
                        } else if constexpr (std::is_same_v<T, client::QuitCommand>) {
                            lifecycle.quit();
                        } else if constexpr (std::is_same_v<T, client::ReconnectCommand>) {
                            static_cast<void>(lifecycle.reconnect());
                        } else if constexpr (std::is_same_v<T, client::WatchCommand>) {
                            presenter.setWatchEnabled(command.enabled);
                            presenter.localMessage(command.enabled ? "watch on" : "watch off");
                        } else if constexpr (std::is_same_v<T, client::RemoteCommand>) {
                            const bool waitingForInitialSynchronization =
                                lifecycle.sessionState() == client::CommandDrainController::SessionState::Connecting ||
                                lifecycle.sessionState() == client::CommandDrainController::SessionState::Synchronizing;
                            const bool accepted = lifecycle.enqueue(std::move(command), retainedInputBytes);
                            if (accepted && waitingForInitialSynchronization && presenter.outputMode() == client::OutputMode::Human) {
                                presenter.localMessage("command queued; waiting for initial synchronization");
                            }
                        } else if constexpr (std::is_same_v<T, client::NewCommand>) {
                            const bool waitingForInitialSynchronization =
                                lifecycle.sessionState() == client::CommandDrainController::SessionState::Connecting ||
                                lifecycle.sessionState() == client::CommandDrainController::SessionState::Synchronizing;
                            const bool accepted = lifecycle.enqueue(std::move(command), retainedInputBytes);
                            if (accepted && waitingForInitialSynchronization && presenter.outputMode() == client::OutputMode::Human) {
                                presenter.localMessage("command queued; waiting for initial synchronization");
                            }
                        } else {
                            lifecycle.localCommandFailed(std::move(command.message));
                        }
                    },
                    std::move(parsed));
            },
            [&lifecycle]() {
                lifecycle.inputEof();
            },
            [&lifecycle](std::string message) {
                lifecycle.inputFailed(std::move(message));
            },
            0,
            [&lifecycle]() {
                lifecycle.localShutdownRequested();
            });
        stdinReader = &input;

        const auto startPersistentStreamClient =
            [&presenter, &lifecycle, &eventLoopRunning, &disconnectedPresented](auto& configuredClient, std::string transport) {
                auto* const flowController = configuredClient.getFlowController();
                configuredClient.connect(
                    [&presenter, &lifecycle, &eventLoopRunning, &disconnectedPresented, flowController, transport = std::move(transport)](
                        const auto&, core::socket::State state) {
                        if (state == core::socket::State::OK || state == core::socket::State::DISABLED) {
                            return;
                        }
                        const std::string failure = "failed to connect using " + transport + ": " + state.what();
                        // SNode.C invokes the status observer before it decides whether
                        // this status will be retried. Inspect the persistent flow on
                        // the next owner-event-loop turn and report only a terminal
                        // cycle; configured framework retry remains transparent here.
                        core::EventReceiver::atNextTick(
                            [&presenter, &lifecycle, &eventLoopRunning, &disconnectedPresented, flowController, failure]() {
                                if (!eventLoopRunning || lifecycle.applicationShutdownActive() || !flowController->isTerminated()) {
                                    return;
                                }
                                lifecycle.connectionAttemptFailed(failure);
                                if (!disconnectedPresented) {
                                    presenter.disconnected();
                                    disconnectedPresented = true;
                                }
                            });
                    });
            };

        bool persistentStreamClientStarted = false;
        const auto selectPersistentStreamClient =
            [&beginConnectionAttempt,
             &prepareExplicitReconnect,
             &stopConfiguredClientFlow,
             &persistentStreamClientStarted,
             &startPersistentStreamClient](auto& configuredClient, client::ClientConnection& connection, std::string transport) {
                beginConnectionAttempt = [&configuredClient,
                                          &connection,
                                          &persistentStreamClientStarted,
                                          &startPersistentStreamClient,
                                          transport = std::move(transport)]() {
                    if (persistentStreamClientStarted && !configuredClient.getFlowController()->isTerminated()) {
                        throw std::runtime_error("the configured frontend client flow is still active");
                    }
                    if (connection.connected()) {
                        throw std::runtime_error("the configured frontend transport is still attached");
                    }
                    persistentStreamClientStarted = true;
                    startPersistentStreamClient(configuredClient, transport);
                };
                prepareExplicitReconnect = [&configuredClient, &connection]() -> std::optional<std::string> {
                    if (connection.connected()) {
                        return "the previous frontend physical connection is still closing";
                    }
                    // The lifecycle/SDK gate has already established Disconnected
                    // with no semantic connection. End any pending SNode.C retry or
                    // reconnect subflow before explicitly starting the next cycle
                    // on this same configured client object.
                    static_cast<void>(configuredClient.getFlowController()->terminateFlow());
                    return std::nullopt;
                };
                stopConfiguredClientFlow = [&configuredClient]() {
                    static_cast<void>(configuredClient.getFlowController()->terminateFlow());
                };
            };

#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        const auto selectPersistentWebSocketClient = [&beginConnectionAttempt,
                                                      &prepareExplicitReconnect,
                                                      &stopConfiguredClientFlow,
                                                      &persistentStreamClientStarted,
                                                      &startPersistentStreamClient,
                                                      webSocketBinding](auto& configuredClient, std::string transport) {
            beginConnectionAttempt = [&configuredClient,
                                      &persistentStreamClientStarted,
                                      &startPersistentStreamClient,
                                      webSocketBinding,
                                      transport = std::move(transport)]() {
                if (persistentStreamClientStarted && !configuredClient.getFlowController()->isTerminated()) {
                    throw std::runtime_error("the configured frontend client flow is still active");
                }
                if (webSocketBinding->connected()) {
                    throw std::runtime_error("the configured frontend WebSocket transport is still attached");
                }
                persistentStreamClientStarted = true;
                startPersistentStreamClient(configuredClient, transport);
            };
            prepareExplicitReconnect = [&configuredClient, webSocketBinding]() -> std::optional<std::string> {
                if (webSocketBinding->connected()) {
                    return "the previous frontend physical connection is still closing";
                }
                static_cast<void>(configuredClient.getFlowController()->terminateFlow());
                return std::nullopt;
            };
            stopConfiguredClientFlow = [&configuredClient]() {
                static_cast<void>(configuredClient.getFlowController()->terminateFlow());
            };
        };
#endif

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
                lifecycle.startupFailed("exactly one outgoing frontend transport must be enabled; found " +
                                        std::to_string(preflight.enabledCount));
                return;
            }

            if (!unixClient.getConfig()->Instance::getDisabled()) {
                selectPersistentStreamClient(unixClient, unixConnection, "Unix JSONL");
            } else if (!ipv4Client.getConfig()->Instance::getDisabled()) {
                selectPersistentStreamClient(ipv4Client, ipv4Connection, "IPv4 JSONL");
            } else if (!ipv6Client.getConfig()->Instance::getDisabled()) {
                selectPersistentStreamClient(ipv6Client, ipv6Connection, "IPv6 JSONL");
            }
#if defined(AISUITE_CODEX_FRONTEND_TLS)
            else if (!tlsIpv4Client.getConfig()->Instance::getDisabled()) {
                selectPersistentStreamClient(tlsIpv4Client, tlsIpv4Connection, "IPv4 TLS JSONL");
            } else if (!tlsIpv6Client.getConfig()->Instance::getDisabled()) {
                selectPersistentStreamClient(tlsIpv6Client, tlsIpv6Connection, "IPv6 TLS JSONL");
            }
#endif
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
            else if (!rfcommClient.getConfig()->Instance::getDisabled()) {
                selectPersistentStreamClient(rfcommClient, rfcommConnection, "RFCOMM JSONL");
            } else if (!rfcommTlsClient.getConfig()->Instance::getDisabled()) {
                selectPersistentStreamClient(rfcommTlsClient, rfcommTlsConnection, "RFCOMM TLS JSONL");
            }
#endif
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
            else if (!webSocketIpv4Client.getConfig()->Instance::getDisabled()) {
                selectPersistentWebSocketClient(webSocketIpv4Client, "WebSocket IPv4");
            } else if (!webSocketIpv6Client.getConfig()->Instance::getDisabled()) {
                selectPersistentWebSocketClient(webSocketIpv6Client, "WebSocket IPv6");
            }
#if defined(AISUITE_CODEX_FRONTEND_TLS)
            else if (!wssIpv4Client.getConfig()->Instance::getDisabled()) {
                selectPersistentWebSocketClient(wssIpv4Client, "WSS IPv4");
            } else if (!wssIpv6Client.getConfig()->Instance::getDisabled()) {
                selectPersistentWebSocketClient(wssIpv6Client, "WSS IPv6");
            }
#endif
#endif
            if (beginConnectionAttempt) {
                disconnectedPresented = false;
                try {
                    beginConnectionAttempt();
                } catch (...) {
                    lifecycle.startupFailed("failed to create the configured frontend transport");
                }
            }
        });

        const int eventLoopResult = core::SNodeC::start();
        eventLoopRunning = false;
        input.stop();
        if (lifecycle.outcome() == client::CommandDrainController::Outcome::Running) {
            lifecycle.quit();
        }
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        webSocketBinding->shutdown();
        webSocketBindingHandle = nullptr;
#endif
        if (stopConfiguredClientFlow) {
            stopConfiguredClientFlow();
        }
        unixConnection.shutdown();
        ipv4Connection.shutdown();
        ipv6Connection.shutdown();
#if defined(AISUITE_CODEX_FRONTEND_TLS)
        tlsIpv4Connection.shutdown();
        tlsIpv6Connection.shutdown();
#endif
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
        rfcommConnection.shutdown();
        rfcommTlsConnection.shutdown();
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
