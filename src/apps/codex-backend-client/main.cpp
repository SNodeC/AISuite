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
#include "web/http/ConfigHttpParser.h"
#include "web/http/ConfigWebSocket.h"
#include "web/http/client/ConfigHTTP.h"
#include "web/http/client/Request.h"
#include "web/http/client/SocketContext.h"
#include "web/http/legacy/in/Client.h"
#include "web/http/legacy/in6/Client.h"
#if defined(AISUITE_CODEX_FRONTEND_TLS)
#include "web/http/tls/in/Client.h"
#include "web/http/tls/in6/Client.h"
#endif
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
#include <vector>

namespace {

    template <typename Config>
    void copyEffectiveSocketConfiguration(Config* source, Config* target) {
        using Remote = typename Config::Remote;
        using Local = typename Config::Local;
        using Connection = typename Config::Connection;
        using Socket = typename Config::Socket;

        auto& sourceRemote = static_cast<Remote&>(*source);
        auto& targetRemote = static_cast<Remote&>(*target);
        targetRemote.setSocketAddress(sourceRemote.getSocketAddress());

        auto& sourceLocal = static_cast<Local&>(*source);
        auto& targetLocal = static_cast<Local&>(*target);
        targetLocal.setSocketAddress(sourceLocal.getSocketAddress());

        auto& sourceConnection = static_cast<Connection&>(*source);
        auto& targetConnection = static_cast<Connection&>(*target);
        targetConnection.setReadTimeout(sourceConnection.getReadTimeout());
        targetConnection.setWriteTimeout(sourceConnection.getWriteTimeout());
        targetConnection.setReadBlockSize(sourceConnection.getReadBlockSize());
        targetConnection.setWriteBlockSize(sourceConnection.getWriteBlockSize());
        targetConnection.setMaximumWriteQueueBytes(sourceConnection.getMaximumWriteQueueBytes());
        targetConnection.setWriteQueueHighWatermark(sourceConnection.getWriteQueueHighWatermark());
        targetConnection.setWriteQueueLowWatermark(sourceConnection.getWriteQueueLowWatermark());
        targetConnection.setTerminateTimeout(sourceConnection.getTerminateTimeout());

        auto& sourceSocket = static_cast<Socket&>(*source);
        auto& targetSocket = static_cast<Socket&>(*target);
        // One application command owns one physical attempt. SNode.C's
        // automatic retry/reconnect flow is deliberately disabled here.
        targetSocket.setRetry(false);
        targetSocket.setRetryOnFatal(false);
        targetSocket.setRetryTimeout(sourceSocket.getRetryTimeout());
        targetSocket.setRetryTries(sourceSocket.getRetryTries());
        targetSocket.setRetryBase(sourceSocket.getRetryBase());
        targetSocket.setRetryLimit(static_cast<unsigned int>(sourceSocket.getRetryLimit()));
        targetSocket.setRetryJitter(sourceSocket.getRetryJitter());
        // Application reconnect is explicit. Never inherit SNode.C's
        // automatic physical reconnect switch into an attempt object.
        targetSocket.setReconnect(false);
        targetSocket.setReconnectTime(sourceSocket.getReconnectTime());
        targetSocket.setConnectTimeout(sourceSocket.getConnectTimeout());
        for (const auto& [level, names] : sourceSocket.getSocketOptions()) {
            for (const auto& [name, option] : names) {
                const char* bytes = static_cast<const char*>(option.getOptValue());
                targetSocket.addSocketOption(level, name, std::vector<char>(bytes, bytes + option.getOptLen()));
            }
        }

        if constexpr (requires { source->getSni(); }) {
            target->setSni(source->getSni());
        }
        if constexpr (requires { source->getCert(); }) {
            target->setInitTimeout(source->getInitTimeout());
            target->setShutdownTimeout(source->getShutdownTimeout());
            target->setCert(source->getCert());
            target->setCertKey(source->getCertKey());
            target->setCertKeyPassword(source->getCertKeyPassword());
            target->setCaCert(source->getCaCert());
            target->setCaCertDir(source->getCaCertDir());
            target->setCaCertDirUseDefault(source->getCaCertDirUseDefault());
            target->setCaCertAcceptUnknown(source->getCaCertAcceptUnknown());
            target->setCipherList(source->getCipherList());
            target->setSslOptions(source->getSslOptions());
            target->setNoCloseNotifyIsEOF(source->getNoCloseNotifyIsEOF());
        }
        target->Instance::setDisabled(false);
    }

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

    template <typename HttpClient>
    void copyEffectiveHttpConfiguration(HttpClient& source, HttpClient& target) {
        copyEffectiveSocketConfiguration(source.getConfig(), target.getConfig());
        auto& sourceInstance = static_cast<net::config::ConfigInstance&>(*source.getConfig());
        auto& targetInstance = static_cast<net::config::ConfigInstance&>(*target.getConfig());
        auto* const sourceHttp = sourceInstance.template getSubCommand<web::http::client::ConfigHTTP>();
        auto* const targetHttp = targetInstance.template getSubCommand<web::http::client::ConfigHTTP>();
        targetHttp->setHostHeader(sourceHttp->getHostHeader());
        targetHttp->setPipelinedRequests(sourceHttp->getPipelinedRequests());
        auto* const sourceParser = sourceHttp->getParserConfig();
        auto* const targetParser = targetHttp->getParserConfig();
        targetParser->setMaximumStartLineBytes(sourceParser->getMaximumStartLineBytes());
        targetParser->setMaximumHeaderLineBytes(sourceParser->getMaximumHeaderLineBytes());
        targetParser->setMaximumHeaderBytes(sourceParser->getMaximumHeaderBytes());
        targetParser->setMaximumHeaderFields(sourceParser->getMaximumHeaderFields());
        targetParser->setMaximumBodyBytes(sourceParser->getMaximumBodyBytes());
        auto* const sourceWebSocket = sourceInstance.template getSubCommand<web::http::ConfigWebSocket>();
        auto* const targetWebSocket = targetInstance.template getSubCommand<web::http::ConfigWebSocket>();
        targetWebSocket->setMaximumFrameBytes(sourceWebSocket->getMaximumFrameBytes());
        targetWebSocket->setMaximumMessageBytes(sourceWebSocket->getMaximumMessageBytes());
        targetWebSocket->setMaximumFragments(sourceWebSocket->getMaximumFragments());
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
        client::FrontendWebSocketClientRuntime* webSocketRuntimeHandle = nullptr;
        client::CommandDrainController* lifecycleHandle = nullptr;
        client::StdinReader* stdinReader = nullptr;
        client::PhysicalConnectionAttemptGate physicalAttempts;
        std::shared_ptr<void> activePhysicalClient;
        std::function<void()> beginConnectionAttempt;
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        std::weak_ptr<web::http::client::MasterRequest> activeWebSocketRequest;
        client::PhysicalConnectionAttemptGate::Generation activeWebSocketRequestGeneration = 0;
        bool webSocketUpgradeCommitted = false;
#endif
        bool disconnectedPresented = false;
        const auto retirePhysicalAttempt = [&physicalAttempts,
                                            &activePhysicalClient](const client::PhysicalConnectionAttemptGate::Generation generation) {
            if (!physicalAttempts.complete(generation)) {
                return;
            }
            std::shared_ptr<void> retired = std::move(activePhysicalClient);
            if (!retired) {
                return;
            }
            // A SNode.C status/disconnect callback may still be unwinding.
            // Keep its owning attempt alive through the current event frame.
            core::EventReceiver::atNextTick([retired = std::move(retired)]() {
            });
        };
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
            if (webSocketRuntimeHandle != nullptr && webSocketRuntimeHandle->connected()) {
                webSocketRuntimeHandle->shutdown();
                return;
            }
            if (const std::shared_ptr<web::http::client::MasterRequest> request = activeWebSocketRequest.lock()) {
                if (closeWebSocketUpgradeTransport(request)) {
                    return;
                }
            }
#endif
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
                     &eventLoopRunning,
                     &disconnectedPresented,
                     &lifecycleHandle,
                     &physicalAttempts,
                     &presenter]() {
                        if (!eventLoopRunning || !beginConnectionAttempt) {
                            return std::optional<std::string>{"configured frontend transport is unavailable"};
                        }
                        if (physicalAttempts.active()) {
                            return std::optional<std::string>{"the previous frontend physical connection is still closing"};
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

        const auto connectionCallbacks =
            [&presenter,
             &lifecycle,
             &eventLoopRunning,
             &connectionHandle,
             &authentication,
             &disconnectedPresented,
             &physicalAttempts,
             &retirePhysicalAttempt](std::string transport, client::ClientConnection** owner, bool verifiedLocalUnix) {
                return client::ClientConnectionCallbacks{
                    .onConnected = {},
                    .onDisconnected = {},
                    .onFailure = {},
                    .onAttemptConnected =
                        [&presenter, &connectionHandle, &disconnectedPresented, &physicalAttempts, owner, transport = std::move(transport)](
                            const client::PhysicalConnectionAttemptGate::Generation generation) {
                            if (!physicalAttempts.isCurrent(generation)) {
                                return;
                            }
                            connectionHandle = *owner;
                            disconnectedPresented = false;
                            presenter.connected(transport);
                            if (presenter.outputMode() == client::OutputMode::Human) {
                                presenter.localMessage("enter 'help' for commands");
                            }
                        },
                    .onAttemptDisconnected =
                        [&presenter,
                         &lifecycle,
                         &eventLoopRunning,
                         &connectionHandle,
                         &disconnectedPresented,
                         &physicalAttempts,
                         &retirePhysicalAttempt,
                         owner](const client::PhysicalConnectionAttemptGate::Generation generation) {
                            if (!physicalAttempts.isCurrent(generation)) {
                                return;
                            }
                            if (connectionHandle == *owner) {
                                connectionHandle = nullptr;
                            }
                            if (!disconnectedPresented) {
                                presenter.disconnected();
                                disconnectedPresented = true;
                            }
                            lifecycle.disconnected();
                            retirePhysicalAttempt(generation);
                            if (eventLoopRunning && lifecycle.applicationShutdownActive()) {
                                core::SNodeC::stop();
                            }
                        },
                    .onAttemptFailure =
                        [&lifecycle, &physicalAttempts](const client::PhysicalConnectionAttemptGate::Generation generation,
                                                        std::string message) {
                            if (!physicalAttempts.isCurrent(generation)) {
                                return;
                            }
                            lifecycle.connectionFailed(std::move(message));
                        },
                    .onOutbound = {},
                    .verifiedLocalUnix = verifiedLocalUnix,
                    .onBeforeTransportConnected =
                        [&authentication](bool localUnix) {
                            authentication.prepare(localUnix);
                        },
                    .onLocalShutdown =
                        [&lifecycle]() {
                            lifecycle.localShutdownRequested();
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
            client::FrontendWebSocketClientCallbacks{
                .onConnected = {},
                .onDisconnected = {},
                .onFailure = {},
                .onAttemptConnected =
                    [&presenter, &disconnectedPresented, &physicalAttempts](const std::uint64_t generation) {
                        if (!physicalAttempts.isCurrent(generation)) {
                            return;
                        }
                        disconnectedPresented = false;
                        presenter.connected("WebSocket/WSS");
                        if (presenter.outputMode() == client::OutputMode::Human) {
                            presenter.localMessage("enter 'help' for commands");
                        }
                    },
                .onAttemptDisconnected =
                    [&presenter,
                     &lifecycle,
                     &eventLoopRunning,
                     &disconnectedPresented,
                     &physicalAttempts,
                     &activeWebSocketRequest,
                     &activeWebSocketRequestGeneration,
                     &webSocketUpgradeCommitted,
                     &retirePhysicalAttempt](const std::uint64_t generation) {
                        if (!physicalAttempts.isCurrent(generation)) {
                            return;
                        }
                        if (activeWebSocketRequestGeneration == generation) {
                            activeWebSocketRequest.reset();
                            activeWebSocketRequestGeneration = 0;
                            webSocketUpgradeCommitted = false;
                        }
                        if (!disconnectedPresented) {
                            presenter.disconnected();
                            disconnectedPresented = true;
                        }
                        lifecycle.disconnected();
                        retirePhysicalAttempt(generation);
                        if (eventLoopRunning && lifecycle.applicationShutdownActive()) {
                            core::SNodeC::stop();
                        }
                    },
                .onAttemptFailure =
                    [&lifecycle, &physicalAttempts](const std::uint64_t generation, std::string message) {
                        if (!physicalAttempts.isCurrent(generation)) {
                            return;
                        }
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
        webSocketRuntimeHandle = &webSocketRuntime;
        if (!webSocketRuntime.install()) {
            throw std::runtime_error("failed to install the frontend WebSocket client runtime");
        }
        client::linkFrontendWebSocketClient();

        const auto beginWebSocketUpgrade =
            [&webSocketRuntime, &physicalAttempts, &activeWebSocketRequest, &activeWebSocketRequestGeneration, &webSocketUpgradeCommitted](
                const client::PhysicalConnectionAttemptGate::Generation generation) {
                return [&webSocketRuntime,
                        &physicalAttempts,
                        &activeWebSocketRequest,
                        &activeWebSocketRequestGeneration,
                        &webSocketUpgradeCommitted,
                        generation](const std::shared_ptr<web::http::client::MasterRequest>& request) {
                    // The HTTP client owns one immutable attempt identity. Never
                    // infer it from mutable application state when this callback
                    // eventually runs: a retired upgrade may finish after the next
                    // explicit attempt has already been prepared.
                    if (!physicalAttempts.isCurrent(generation) || !webSocketRuntime.isCurrentAttempt(generation)) {
                        static_cast<void>(closeWebSocketUpgradeTransport(request));
                        return;
                    }
                    const auto* const transport = request != nullptr && request->getSocketContext() != nullptr
                                                      ? request->getSocketContext()->getSocketConnection()
                                                      : nullptr;
                    if (!webSocketRuntime.bindAttemptTransport(generation, transport)) {
                        static_cast<void>(closeWebSocketUpgradeTransport(request));
                        return;
                    }
                    activeWebSocketRequest = request;
                    activeWebSocketRequestGeneration = generation;
                    webSocketUpgradeCommitted = false;
                    const std::weak_ptr<web::http::client::MasterRequest> requestWeak = request;
                    const auto isCurrentUpgrade =
                        [&physicalAttempts, &activeWebSocketRequest, &activeWebSocketRequestGeneration, requestWeak, generation]() {
                            const std::shared_ptr<web::http::client::MasterRequest> expected = requestWeak.lock();
                            const std::shared_ptr<web::http::client::MasterRequest> active = activeWebSocketRequest.lock();
                            return expected != nullptr && active == expected && activeWebSocketRequestGeneration == generation &&
                                   physicalAttempts.isCurrent(generation);
                        };
                    const auto failUpgrade = [&webSocketRuntime, isCurrentUpgrade, requestWeak, generation](std::string message) {
                        if (!isCurrentUpgrade()) {
                            return;
                        }
                        webSocketRuntime.reportAttemptFailure(generation, std::move(message));
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
                        [failUpgrade, isCurrentUpgrade, &webSocketUpgradeCommitted](const auto&, const auto& response, bool success) {
                            if (!success || response->get("upgrade") != "websocket" || response->get("sec-websocket-protocol") != "codex") {
                                failUpgrade("frontend WebSocket upgrade was rejected");
                                return;
                            }
                            if (isCurrentUpgrade()) {
                                webSocketUpgradeCommitted = true;
                            }
                        },
                        [failUpgrade](const auto&, const std::string& message) {
                            failUpgrade("frontend WebSocket HTTP response failed: " + message);
                        });
                };
            };
        const auto endWebSocketHttp = [&presenter,
                                       &lifecycle,
                                       &eventLoopRunning,
                                       &disconnectedPresented,
                                       &physicalAttempts,
                                       &activeWebSocketRequest,
                                       &activeWebSocketRequestGeneration,
                                       &webSocketUpgradeCommitted,
                                       &webSocketRuntime,
                                       &retirePhysicalAttempt](const client::PhysicalConnectionAttemptGate::Generation generation) {
            return [&presenter,
                    &lifecycle,
                    &eventLoopRunning,
                    &disconnectedPresented,
                    &physicalAttempts,
                    &activeWebSocketRequest,
                    &activeWebSocketRequestGeneration,
                    &webSocketUpgradeCommitted,
                    &webSocketRuntime,
                    &retirePhysicalAttempt,
                    generation](const std::shared_ptr<web::http::client::MasterRequest>& request) {
                const std::shared_ptr<web::http::client::MasterRequest> activeRequest = activeWebSocketRequest.lock();
                if ((activeRequest && activeRequest != request) || !physicalAttempts.isCurrent(generation) ||
                    activeWebSocketRequestGeneration != generation) {
                    return;
                }
                activeWebSocketRequest.reset();
                activeWebSocketRequestGeneration = 0;
                if (webSocketUpgradeCommitted || webSocketRuntime.connected()) {
                    return;
                }
                webSocketRuntime.abandonAttempt(generation);
                if (!disconnectedPresented) {
                    presenter.disconnected();
                    disconnectedPresented = true;
                }
                lifecycle.disconnected();
                retirePhysicalAttempt(generation);
                if (eventLoopRunning && lifecycle.applicationShutdownActive()) {
                    core::SNodeC::stop();
                }
            };
        };

        web::http::legacy::in::Client webSocketIpv4Client(
            "codex-backend-client-websocket-ipv4", beginWebSocketUpgrade(0), endWebSocketHttp(0));
        webSocketIpv4Client.getConfig()->Instance::setDisabled(true);
        webSocketIpv4Client.getConfig()->Remote::setHost("127.0.0.1");
        webSocketIpv4Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

        web::http::legacy::in6::Client webSocketIpv6Client(
            "codex-backend-client-websocket-ipv6", beginWebSocketUpgrade(0), endWebSocketHttp(0));
        webSocketIpv6Client.getConfig()->Instance::setDisabled(true);
        webSocketIpv6Client.getConfig()->Remote::setHost("::1");
        webSocketIpv6Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

#if defined(AISUITE_CODEX_FRONTEND_TLS)
        web::http::tls::in::Client wssIpv4Client("codex-backend-client-wss-ipv4", beginWebSocketUpgrade(0), endWebSocketHttp(0));
        wssIpv4Client.getConfig()->Instance::setDisabled(true);
        wssIpv4Client.getConfig()->Remote::setHost("127.0.0.1");
        wssIpv4Client.getConfig()->Connection::setMaximumWriteQueueBytes(client::DEFAULT_MAXIMUM_OUTBOUND_BYTES);

        web::http::tls::in6::Client wssIpv6Client("codex-backend-client-wss-ipv6", beginWebSocketUpgrade(0), endWebSocketHttp(0));
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

        const auto reportConnection =
            [&presenter, &lifecycle, &eventLoopRunning, &disconnectedPresented, &physicalAttempts, &retirePhysicalAttempt](
                std::string transport,
                const client::PhysicalConnectionAttemptGate::Generation generation,
                std::function<void()> cancelPrepared,
                std::function<bool()> hasPhysicalAttachment) {
                return [&presenter,
                        &lifecycle,
                        &eventLoopRunning,
                        &disconnectedPresented,
                        &physicalAttempts,
                        &retirePhysicalAttempt,
                        generation,
                        cancelPrepared = std::move(cancelPrepared),
                        hasPhysicalAttachment = std::move(hasPhysicalAttachment),
                        transport = std::move(transport)](const auto&, core::socket::State state) {
                    if (state != core::socket::State::OK && state != core::socket::State::DISABLED) {
                        if (!physicalAttempts.isCurrent(generation)) {
                            return;
                        }
                        lifecycle.connectionAttemptFailed("failed to connect using " + transport + ": " + state.what());
                        if (!lifecycle.applicationShutdownActive() && !disconnectedPresented) {
                            presenter.disconnected();
                            disconnectedPresented = true;
                        }
                        if (!hasPhysicalAttachment()) {
                            cancelPrepared();
                            retirePhysicalAttempt(generation);
                        }
                        if (eventLoopRunning && lifecycle.applicationShutdownActive()) {
                            core::SNodeC::stop();
                        }
                    }
                };
            };

        const auto startStreamAttempt = [&physicalAttempts, &activePhysicalClient, &retirePhysicalAttempt, &reportConnection](
                                            auto& configuredClient, client::ClientConnection& connection, std::string transport) {
            const std::optional<client::PhysicalConnectionAttemptGate::Generation> generation = physicalAttempts.begin();
            if (!generation) {
                throw std::runtime_error("a frontend physical connection attempt is already active");
            }
            if (!connection.prepareAttempt(*generation)) {
                static_cast<void>(physicalAttempts.complete(*generation));
                throw std::runtime_error("the configured frontend transport is still attached");
            }
            try {
                using Attempt = std::remove_reference_t<decltype(configuredClient)>;
                auto clientAttempt = std::make_shared<Attempt>("", connection);
                copyEffectiveSocketConfiguration(configuredClient.getConfig(), clientAttempt->getConfig());
                activePhysicalClient = clientAttempt;
                clientAttempt->connect(reportConnection(
                    std::move(transport),
                    *generation,
                    [&connection, generation = *generation]() {
                        connection.cancelPreparedAttempt(generation);
                    },
                    [&connection, generation = *generation]() {
                        return connection.hasAttachment(generation);
                    }));
            } catch (...) {
                connection.cancelPreparedAttempt(*generation);
                retirePhysicalAttempt(*generation);
                throw;
            }
        };

#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        const auto startWebSocketAttempt = [&physicalAttempts,
                                            &activePhysicalClient,
                                            &activeWebSocketRequestGeneration,
                                            &webSocketRuntime,
                                            &retirePhysicalAttempt,
                                            &reportConnection,
                                            &beginWebSocketUpgrade,
                                            &endWebSocketHttp](auto& configuredClient, std::string transport) {
            const std::optional<client::PhysicalConnectionAttemptGate::Generation> generation = physicalAttempts.begin();
            if (!generation) {
                throw std::runtime_error("a frontend physical connection attempt is already active");
            }
            if (!webSocketRuntime.prepareAttempt(*generation)) {
                static_cast<void>(physicalAttempts.complete(*generation));
                throw std::runtime_error("the configured frontend WebSocket transport is still attached");
            }
            try {
                using Attempt = std::remove_reference_t<decltype(configuredClient)>;
                auto clientAttempt = std::make_shared<Attempt>("", beginWebSocketUpgrade(*generation), endWebSocketHttp(*generation));
                copyEffectiveHttpConfiguration(configuredClient, *clientAttempt);
                activePhysicalClient = clientAttempt;
                clientAttempt->connect(reportConnection(
                    std::move(transport),
                    *generation,
                    [&webSocketRuntime, generation = *generation]() {
                        webSocketRuntime.abandonAttempt(generation);
                    },
                    [&activeWebSocketRequestGeneration, &webSocketRuntime, generation = *generation]() {
                        return activeWebSocketRequestGeneration == generation || webSocketRuntime.connected();
                    }));
            } catch (...) {
                webSocketRuntime.abandonAttempt(*generation);
                retirePhysicalAttempt(*generation);
                throw;
            }
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
                beginConnectionAttempt = [&]() {
                    startStreamAttempt(unixClient, unixConnection, "Unix JSONL");
                };
            } else if (!ipv4Client.getConfig()->Instance::getDisabled()) {
                beginConnectionAttempt = [&]() {
                    startStreamAttempt(ipv4Client, ipv4Connection, "IPv4 JSONL");
                };
            } else if (!ipv6Client.getConfig()->Instance::getDisabled()) {
                beginConnectionAttempt = [&]() {
                    startStreamAttempt(ipv6Client, ipv6Connection, "IPv6 JSONL");
                };
            }
#if defined(AISUITE_CODEX_FRONTEND_TLS)
            else if (!tlsIpv4Client.getConfig()->Instance::getDisabled()) {
                beginConnectionAttempt = [&]() {
                    startStreamAttempt(tlsIpv4Client, tlsIpv4Connection, "IPv4 TLS JSONL");
                };
            } else if (!tlsIpv6Client.getConfig()->Instance::getDisabled()) {
                beginConnectionAttempt = [&]() {
                    startStreamAttempt(tlsIpv6Client, tlsIpv6Connection, "IPv6 TLS JSONL");
                };
            }
#endif
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
            else if (!rfcommClient.getConfig()->Instance::getDisabled()) {
                beginConnectionAttempt = [&]() {
                    startStreamAttempt(rfcommClient, rfcommConnection, "RFCOMM JSONL");
                };
            } else if (!rfcommTlsClient.getConfig()->Instance::getDisabled()) {
                beginConnectionAttempt = [&]() {
                    startStreamAttempt(rfcommTlsClient, rfcommTlsConnection, "RFCOMM TLS JSONL");
                };
            }
#endif
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
            else if (!webSocketIpv4Client.getConfig()->Instance::getDisabled()) {
                beginConnectionAttempt = [&]() {
                    startWebSocketAttempt(webSocketIpv4Client, "WebSocket IPv4");
                };
            } else if (!webSocketIpv6Client.getConfig()->Instance::getDisabled()) {
                beginConnectionAttempt = [&]() {
                    startWebSocketAttempt(webSocketIpv6Client, "WebSocket IPv6");
                };
            }
#if defined(AISUITE_CODEX_FRONTEND_TLS)
            else if (!wssIpv4Client.getConfig()->Instance::getDisabled()) {
                beginConnectionAttempt = [&]() {
                    startWebSocketAttempt(wssIpv4Client, "WSS IPv4");
                };
            } else if (!wssIpv6Client.getConfig()->Instance::getDisabled()) {
                beginConnectionAttempt = [&]() {
                    startWebSocketAttempt(wssIpv6Client, "WSS IPv6");
                };
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
