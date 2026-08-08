/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CodexBackendTestSupport.h"
#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/frontend/FrontendService.h"
#include "ai/openai/codex/frontend/client/Client.h"
#include "ai/openai/codex/frontend/client/Controller.h"
#include "apps/codex-backend-client/ClientConnection.h"
#include "apps/codex-backend-client/FrontendWebSocketClient.h"
#include "apps/codex-backend/FrontendRuntimeBridge.h"
#include "apps/codex-backend/FrontendWebApplication.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/socket/SocketAddress.h"
#include "core/socket/State.h"
#include "core/socket/stream/SocketConnection.h"
#include "core/timer/Timer.h"
#include "express/legacy/in/WebApp.h"
#include "express/legacy/in6/WebApp.h"
#if defined(AISUITE_CODEX_WEBSOCKET_ACCEPTANCE_TLS)
#include "express/tls/in/WebApp.h"
#include "express/tls/in6/WebApp.h"
#endif
#include "net/in/SocketAddress.h"
#include "net/in6/SocketAddress.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"
#include "web/http/ConfigHttpParser.h"
#include "web/http/ConfigWebSocket.h"
#include "web/http/client/ConfigHTTP.h"
#include "web/http/client/Request.h"
#include "web/http/client/SocketContext.h"
#include "web/http/legacy/in/Client.h"
#include "web/http/legacy/in6/Client.h"
#include "web/http/server/ConfigHttpServer.h"
#if defined(AISUITE_CODEX_WEBSOCKET_ACCEPTANCE_TLS)
#include "web/http/tls/in/Client.h"
#include "web/http/tls/in6/Client.h"
#endif

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace backend = ai::openai::codex::backend;
    namespace frontend = ai::openai::codex::frontend;
    namespace sdk = ai::openai::codex::frontend::client;
    namespace backend_app = apps::codex_backend;
    namespace client_app = apps::codex_backend_client;

    using FakeBackendCore = backend::BackendCore<tests::codex::FakeAppServerClient>;

    constexpr std::string_view Endpoint = "/frontend";
    constexpr std::string_view Bearer = "a1-7c-1-cli-websocket-acceptance";
    constexpr std::size_t MaximumMessageBytes = 1024U * 1024U;

    enum class Transport { WebSocketIpv4, WebSocketIpv6, WssIpv4, WssIpv6 };

    std::optional<Transport> parseTransport(std::string_view name) {
        if (name == "websocket-ipv4") {
            return Transport::WebSocketIpv4;
        }
        if (name == "websocket-ipv6") {
            return Transport::WebSocketIpv6;
        }
        if (name == "wss-ipv4") {
            return Transport::WssIpv4;
        }
        if (name == "wss-ipv6") {
            return Transport::WssIpv6;
        }
        return std::nullopt;
    }

    bool usesIpv6(Transport transport) noexcept {
        return transport == Transport::WebSocketIpv6 || transport == Transport::WssIpv6;
    }

    [[maybe_unused]] bool usesTls(Transport transport) noexcept {
        return transport == Transport::WssIpv4 || transport == Transport::WssIpv6;
    }

    std::string_view transportName(Transport transport) noexcept {
        switch (transport) {
            case Transport::WebSocketIpv4:
                return "IPv4 WebSocket";
            case Transport::WebSocketIpv6:
                return "IPv6 WebSocket";
            case Transport::WssIpv4:
                return "IPv4 WSS";
            case Transport::WssIpv6:
                return "IPv6 WSS";
        }
        return "unknown";
    }

    bool expectedIpv6ResolverAbsence(int error) noexcept {
        if (error == EAI_NONAME || error == EAI_FAMILY) {
            return true;
        }
#if defined(EAI_ADDRFAMILY)
        if (error == EAI_ADDRFAMILY) {
            return true;
        }
#endif
        return false;
    }

    bool expectedIpv6SocketAbsence(int error) noexcept {
        return error == EAFNOSUPPORT || error == EPROTONOSUPPORT || error == EADDRNOTAVAIL;
    }

    bool ipv6LoopbackAvailable(tests::support::TestResult& result) {
        try {
            net::in6::SocketAddress address("::1", 0);
            address.init({.aiFlags = AI_PASSIVE | AI_NUMERICHOST, .aiSockType = SOCK_STREAM, .aiProtocol = IPPROTO_TCP});
        } catch (const core::socket::SocketAddress::BadSocketAddress& error) {
            if (expectedIpv6ResolverAbsence(error.getErrnum())) {
                std::cout << "IPv6 WebSocket CLI adapter loopback skipped: SNode.C AI_ADDRCONFIG cannot resolve ::1.\n";
                return false;
            }
            result.expectTrue(false, std::string("unexpected IPv6 resolver failure: ") + error.what());
            return false;
        }

        const int descriptor = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (descriptor < 0) {
            if (expectedIpv6SocketAbsence(errno)) {
                std::cout << "IPv6 WebSocket CLI adapter loopback skipped: this platform has no IPv6 stream socket.\n";
                return false;
            }
            result.expectTrue(false, "unexpected IPv6 socket probe failure");
            return false;
        }
        sockaddr_in6 loopback{};
        loopback.sin6_family = AF_INET6;
        loopback.sin6_addr = in6addr_loopback;
        const int bindResult = ::bind(descriptor, reinterpret_cast<const sockaddr*>(&loopback), sizeof(loopback));
        const int bindError = errno;
        static_cast<void>(::close(descriptor));
        if (bindResult == 0) {
            return true;
        }
        if (expectedIpv6SocketAbsence(bindError)) {
            std::cout << "IPv6 WebSocket CLI adapter loopback skipped: this platform cannot bind ::1.\n";
            return false;
        }
        result.expectTrue(false, "unexpected IPv6 bind probe failure");
        return false;
    }

    struct ScenarioState {
        ScenarioState(tests::support::TestResult& result, Transport transport)
            : result(result)
            , transport(transport) {
        }

        void fail(std::string message) {
            ++failures;
            result.expectTrue(false, std::move(message));
            core::SNodeC::stop();
        }

        tests::support::TestResult& result;
        Transport transport;
        client_app::FrontendWebSocketClientRuntime* runtime = nullptr;
        sdk::Client* sdkClient = nullptr;
        core::socket::stream::SocketConnection* transportConnection = nullptr;
        std::optional<frontend::FrontendPeerContext> authenticatedPeer;
        bool timedOut = false;
        bool synchronizedWhileConnected = false;
        std::size_t failures = 0;
        std::size_t listenerBound = 0;
        std::size_t httpConnected = 0;
        std::size_t upgradeStarted = 0;
        std::size_t upgradeCompleted = 0;
        std::size_t runtimeConnected = 0;
        std::size_t runtimeDisconnected = 0;
        std::size_t authenticationPrepared = 0;
        std::size_t authenticationAttempts = 0;
        std::size_t welcomeReceived = 0;
        std::size_t snapshotReceived = 0;
        std::size_t syncCompleteReceived = 0;
        std::size_t synchronized = 0;
        std::size_t readyTransitions = 0;
        std::size_t localShutdowns = 0;
        std::size_t unintentionalDisconnects = 0;
        std::size_t staleHttpCallbacks = 0;
        std::size_t attemptConnected = 0;
        std::size_t attemptDisconnected = 0;
        std::size_t overlapRejections = 0;
        std::size_t postReconnectCommands = 0;
        std::vector<std::string> sessionIds;
    };

    frontend::AuthenticationResult
    authenticate(ScenarioState& state, const frontend::FrontendPeerContext& peer, const frontend::AuthenticationCredential& credential) {
        ++state.authenticationAttempts;
        state.authenticatedPeer = peer;
        const auto* bearer = std::get_if<frontend::BearerCredential>(&credential);
        if (bearer == nullptr || bearer->token != Bearer) {
            return frontend::AuthenticationFailure{bearer == nullptr ? frontend::AuthenticationFailureCode::AuthenticationRequired
                                                                     : frontend::AuthenticationFailureCode::AuthenticationFailed};
        }
        return frontend::AuthenticationSuccess{frontend::FrontendPrincipal{
            "cli-websocket-acceptance",
            std::vector<frontend::FrontendScope>(frontend::DefaultRemoteScopes.begin(), frontend::DefaultRemoteScopes.end()),
            "default_remote",
            false}};
    }

    template <bool Encrypted, typename WebApp, typename HttpClient, typename Address>
    int runLoopback([[maybe_unused]] int argc,
                    char* argv[],
                    tests::support::TestResult& result,
                    Transport transport,
                    const Address& listenAddress,
                    const std::string& connectHost) {
        core::SNodeC::init(1, argv);
        ScenarioState state(result, transport);
        int eventLoopResult = 1;
        {
            const auto fakeTransport = std::make_shared<tests::codex::FakeTransportState>();
            FakeBackendCore backend({}, fakeTransport);
            frontend::FrontendServiceOptions serviceOptions;
            serviceOptions.authenticator = [&state](const frontend::FrontendPeerContext& peer,
                                                    const frontend::AuthenticationCredential& credential) {
                return authenticate(state, peer, credential);
            };
            frontend::FrontendService service(backend, std::move(serviceOptions));
            result.expectTrue(backend_app::installFrontendRuntime(service),
                              "the production server WebSocket plugin bridge installs exactly once");

            backend_app::FrontendWebApplication webApplication(
                service,
                backend_app::FrontendWebApplicationOptions{.endpoint = std::string(Endpoint),
                                                           .staticRoot = std::nullopt,
                                                           .allowedOrigins = {},
                                                           .transport = Encrypted ? frontend::FrontendTransportKind::WebSocketTls
                                                                                  : frontend::FrontendTransportKind::WebSocket,
                                                           .encrypted = Encrypted});
            WebApp webApp("a1-7c-1-cli-websocket-acceptance-server");
            webApplication.configure(webApp);
            webApp.getConfig()->Instance::forceUnrequired();
            if constexpr (Encrypted) {
#if defined(AISUITE_CODEX_WEBSOCKET_ACCEPTANCE_TLS)
                webApp.getConfig()->Tls::setCert(AISUITE_CODEX_TEST_TLS_CERT);
                webApp.getConfig()->Tls::setCertKey(AISUITE_CODEX_TEST_TLS_KEY);
#endif
            }
            auto* httpPolicy = webApp.getConfig()->net::config::ConfigInstance::getSubCommand<web::http::server::ConfigHttpServer>();
            httpPolicy->setMaximumPendingRequests(1)->setAllowChunkedTransfer(false)->setAllowPipelining(false);
            httpPolicy->getParserConfig()
                ->setMaximumStartLineBytes(8192)
                ->setMaximumHeaderLineBytes(8192)
                ->setMaximumHeaderBytes(65536)
                ->setMaximumHeaderFields(128)
                ->setMaximumBodyBytes(1);
            webApp.getConfig()
                ->net::config::ConfigInstance::getSubCommand<web::http::ConfigWebSocket>()
                ->setMaximumFrameBytes(MaximumMessageBytes)
                ->setMaximumMessageBytes(MaximumMessageBytes)
                ->setMaximumFragments(4096);

            sdk::ClientOptions options;
            options.credentialProvider = [] {
                return sdk::AuthenticationContext{frontend::BearerCredential{std::string(Bearer)}, "websocket-profile:test"};
            };
            sdk::Client client(
                std::move(options),
                sdk::ClientCallbacks{
                    .onConnectionStateChanged =
                        [&state](const sdk::ConnectionStateChange& change) {
                            if (change.current == sdk::ConnectionState::Ready) {
                                ++state.readyTransitions;
                            }
                        },
                    .onStateUpdated = {},
                    .onSynchronized =
                        [&state](const sdk::SynchronizationInfo&) {
                            ++state.synchronized;
                            state.synchronizedWhileConnected = state.runtime != nullptr && state.runtime->connected();
                            core::EventReceiver::atNextTick([&state] {
                                if (state.runtime == nullptr || !state.runtime->connected()) {
                                    state.fail("production CLI WebSocket runtime detached before synchronized work interleaved");
                                    return;
                                }
                                if (state.synchronized == 1) {
                                    if (state.transportConnection == nullptr) {
                                        state.fail("the first WebSocket transport disappeared before forced remote-style loss");
                                        return;
                                    }
                                    // Close the physical socket underneath the
                                    // production subprotocol. Unlike
                                    // runtime.shutdown(), this does not mark an
                                    // application-local shutdown and therefore
                                    // exercises the remote/unintentional loss
                                    // disposition used by explicit reconnect.
                                    state.transportConnection->close();
                                    return;
                                }
                                if (state.synchronized != 2 || state.sdkClient == nullptr || !state.sdkClient->isReady() ||
                                    state.sdkClient->controller().ownedByThisClient() || !state.sdkClient->session() ||
                                    state.sdkClient->session()->role != frontend::SessionRole::Observer) {
                                    state.fail("the second WebSocket attachment did not reach a fresh observer Ready session");
                                    return;
                                }
                                const sdk::Submission submission = state.sdkClient->controller().acquire(
                                    [&state](const sdk::OperationResult<sdk::ControllerResult>& operation) {
                                        ++state.postReconnectCommands;
                                        state.result.expectTrue(operation && operation.value->ownedByThisClient,
                                                                "a native typed command succeeds after WebSocket reconnect");
                                        core::EventReceiver::atNextTick([&state] {
                                            if (state.transport == Transport::WebSocketIpv4) {
                                                static_cast<void>(::kill(::getpid(), SIGINT));
                                            } else if (state.runtime != nullptr) {
                                                state.runtime->shutdown();
                                            }
                                        });
                                    });
                                state.result.expectTrue(submission.accepted(), "the reconnected WebSocket session accepts a new command");
                            });
                        },
                    .onCursorAdvanced = {},
                    .onProtocolMessage =
                        [&state](const frontend::ServerMessage& message) {
                            if (const auto* welcome = std::get_if<frontend::Welcome>(&message)) {
                                ++state.welcomeReceived;
                                state.sessionIds.push_back(welcome->sessionId);
                            }
                            state.snapshotReceived += std::holds_alternative<frontend::Snapshot>(message) ? 1U : 0U;
                            state.syncCompleteReceived += std::holds_alternative<frontend::SyncComplete>(message) ? 1U : 0U;
                        },
                    .onDiagnostic = {}});
            state.sdkClient = &client;
            client_app::PhysicalConnectionAttemptGate physicalAttempts;
            std::function<void()> startAttempt;
            std::shared_ptr<HttpClient> httpClient;
            std::weak_ptr<web::http::client::MasterRequest> activeHttpRequest;
            client_app::PhysicalConnectionAttemptGate::Generation activeHttpGeneration = 0;
            bool upgradeCommitted = false;

            client_app::FrontendWebSocketClientRuntime runtime(
                client,
                client_app::FrontendWebSocketClientCallbacks{
                    .onConnected =
                        [&state] {
                            ++state.runtimeConnected;
                        },
                    .onDisconnected =
                        [&state] {
                            ++state.runtimeDisconnected;
                        },
                    .onFailure =
                        [&state](std::string message) {
                            state.fail(std::string(transportName(state.transport)) + " runtime failed: " + message);
                        },
                    .onAttemptConnected =
                        [&state, &physicalAttempts](const std::uint64_t generation) {
                            ++state.attemptConnected;
                            state.result.expectTrue(physicalAttempts.isCurrent(generation),
                                                    "only the current WebSocket generation may attach to the SDK");
                        },
                    .onAttemptDisconnected =
                        [&state, &physicalAttempts, &httpClient, &startAttempt](const std::uint64_t generation) {
                            if (!physicalAttempts.isCurrent(generation)) {
                                state.fail("a stale WebSocket detach callback reached application lifecycle state");
                                return;
                            }
                            ++state.attemptDisconnected;
                            if (state.attemptDisconnected == 1) {
                                state.result.expectTrue(
                                    state.localShutdowns == 0,
                                    "the first WebSocket loss is unintentional and does not request application shutdown");
                                ++state.unintentionalDisconnects;
                            }
                            static_cast<void>(physicalAttempts.complete(generation));
                            state.transportConnection = nullptr;
                            std::shared_ptr<HttpClient> retired = std::move(httpClient);
                            if (state.attemptDisconnected == 1) {
                                core::EventReceiver::atNextTick([retired = std::move(retired), &startAttempt] {
                                    startAttempt();
                                });
                            } else {
                                core::EventReceiver::atNextTick([retired = std::move(retired)] {
                                    core::SNodeC::stop();
                                });
                            }
                        },
                    .onAttemptFailure =
                        [&state, &physicalAttempts](const std::uint64_t generation, std::string message) {
                            if (physicalAttempts.isCurrent(generation)) {
                                state.fail(std::string(transportName(state.transport)) + " current attempt failed: " + message);
                            }
                        },
                    .onBeforeTransportConnected =
                        [&state](bool localUnix) {
                            ++state.authenticationPrepared;
                            state.result.expectTrue(!localUnix, "WebSocket/WSS always uses remote authentication preparation");
                        },
                    .onLocalShutdown =
                        [&state] {
                            ++state.localShutdowns;
                        }});
            state.runtime = &runtime;
            result.expectTrue(runtime.install(), "the production CLI WebSocket runtime bridge installs exactly once");
            client_app::linkFrontendWebSocketClient();

            webApp.listen(listenAddress, [&](const Address& bound, core::socket::State listenState) {
                if (listenState != core::socket::State::OK || bound.getPort() == 0) {
                    state.fail(std::string(transportName(transport)) + " server failed to listen: " + listenState.what());
                    return;
                }
                ++state.listenerBound;
                const Address remote(connectHost, bound.getPort());
                const std::uint16_t port = bound.getPort();
                startAttempt = [&, remote, port] {
                    const auto generation = physicalAttempts.begin();
                    if (!generation || !runtime.prepareAttempt(*generation)) {
                        state.fail("the configured WebSocket adapter allowed overlapping physical attempts");
                        return;
                    }
                    if (!physicalAttempts.begin()) {
                        ++state.overlapRejections;
                    }
                    const auto beginUpgrade = [&,
                                               generation = *generation](const std::shared_ptr<web::http::client::MasterRequest>& request) {
                        if (!physicalAttempts.isCurrent(generation) || !runtime.isCurrentAttempt(generation)) {
                            ++state.staleHttpCallbacks;
                            if (request != nullptr && request->getSocketContext() != nullptr) {
                                request->getSocketContext()->close();
                            }
                            return;
                        }
                        auto* const transport = request != nullptr && request->getSocketContext() != nullptr
                                                    ? request->getSocketContext()->getSocketConnection()
                                                    : nullptr;
                        if (!runtime.bindAttemptTransport(generation, transport)) {
                            state.fail("the WebSocket runtime rejected its originating HTTP transport identity");
                            if (request != nullptr && request->getSocketContext() != nullptr) {
                                request->getSocketContext()->close();
                            }
                            return;
                        }
                        activeHttpRequest = request;
                        activeHttpGeneration = generation;
                        upgradeCommitted = false;
                        state.transportConnection = transport;
                        const std::weak_ptr<web::http::client::MasterRequest> requestWeak = request;
                        const auto currentUpgrade = [&, requestWeak, generation] {
                            const std::shared_ptr<web::http::client::MasterRequest> expected = requestWeak.lock();
                            const std::shared_ptr<web::http::client::MasterRequest> active = activeHttpRequest.lock();
                            return expected != nullptr && active == expected && activeHttpGeneration == generation &&
                                   physicalAttempts.isCurrent(generation);
                        };
                        request->set("Sec-WebSocket-Protocol", "codex");
                        request->upgrade(
                            std::string(Endpoint),
                            "websocket",
                            [&state, currentUpgrade](bool success) {
                                if (!currentUpgrade()) {
                                    ++state.staleHttpCallbacks;
                                } else if (success) {
                                    ++state.upgradeStarted;
                                } else {
                                    state.fail("production CLI WebSocket upgrade could not be initiated");
                                }
                            },
                            [&state, &upgradeCommitted, currentUpgrade](const auto&, const auto& response, bool success) {
                                if (!currentUpgrade()) {
                                    ++state.staleHttpCallbacks;
                                } else if (success && response->get("upgrade") == "websocket" &&
                                           response->get("sec-websocket-protocol") == "codex") {
                                    upgradeCommitted = true;
                                    ++state.upgradeCompleted;
                                } else {
                                    state.fail("production CLI WebSocket upgrade response was rejected (status=" + response->statusCode +
                                               ", upgrade=" + response->get("upgrade") +
                                               ", subprotocol=" + response->get("sec-websocket-protocol") + ")");
                                }
                            },
                            [&state, currentUpgrade](const auto&, const std::string& message) {
                                if (!currentUpgrade()) {
                                    ++state.staleHttpCallbacks;
                                } else {
                                    state.fail("production CLI WebSocket HTTP response failed: " + message);
                                }
                            });
                    };
                    const auto endHttp = [&, generation = *generation](const std::shared_ptr<web::http::client::MasterRequest>& request) {
                        const std::shared_ptr<web::http::client::MasterRequest> active = activeHttpRequest.lock();
                        if ((active && active != request) || !physicalAttempts.isCurrent(generation) ||
                            activeHttpGeneration != generation) {
                            ++state.staleHttpCallbacks;
                            return;
                        }
                        activeHttpRequest.reset();
                        activeHttpGeneration = 0;
                        if (upgradeCommitted || runtime.connected()) {
                            return;
                        }
                        runtime.abandonAttempt(generation);
                    };
                    httpClient = std::make_shared<HttpClient>("", std::move(beginUpgrade), std::move(endHttp));
                    httpClient->getConfig()->Instance::forceUnrequired();
                    if (usesIpv6(state.transport)) {
                        // SNode.C 2.0 derives the default HTTP Host field from
                        // SocketAddress::toString(false), which does not bracket
                        // IPv6 literals. Exercise its public HTTP client policy
                        // override until the framework formats IPv6 authorities.
                        httpClient->getConfig()
                            ->net::config::ConfigInstance::template getSubCommand<web::http::client::ConfigHttpClient>()
                            ->setHostHeader("[" + connectHost + "]:" + std::to_string(port));
                    }
                    if constexpr (Encrypted) {
#if defined(AISUITE_CODEX_WEBSOCKET_ACCEPTANCE_TLS)
                        httpClient->getConfig()->Tls::setCaCert(AISUITE_CODEX_TEST_TLS_CERT);
                        httpClient->getConfig()->Tls::setCaCertAcceptUnknown(false);
                        httpClient->getConfig()->Tls::setSni("localhost");
#endif
                    }
                    httpClient->connect(remote, [&state](const Address&, core::socket::State connectState) {
                        if (connectState == core::socket::State::OK) {
                            ++state.httpConnected;
                        } else {
                            state.fail(std::string(transportName(state.transport)) + " HTTP client failed: " + connectState.what());
                        }
                    });
                };
                startAttempt();
            });

            [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
                [&state] {
                    state.timedOut = true;
                    state.fail(std::string(transportName(state.transport)) + " acceptance timed out");
                },
                utils::Timeval({5, 0}));
            eventLoopResult = core::SNodeC::start(utils::Timeval({7, 0}));
            httpClient.reset();
            runtime.uninstall();
            state.runtime = nullptr;
            state.sdkClient = nullptr;
            service.close("CLI WebSocket acceptance complete");
            backend_app::uninstallFrontendRuntime(service);
        }
        core::SNodeC::free();

        const int expectedLoopResult = transport == Transport::WebSocketIpv4 ? -SIGINT : 0;
        result.expectTrue(eventLoopResult == expectedLoopResult && !state.timedOut && state.failures == 0,
                          std::string(transportName(transport)) + " completes two deterministic production adapter lifecycles");
        result.expectTrue(state.listenerBound == 1 && state.httpConnected == 2 && state.upgradeStarted == 2 &&
                              state.upgradeCompleted == 2 && state.runtimeConnected == 2 && state.runtimeDisconnected == 2 &&
                              state.attemptConnected == 2 && state.attemptDisconnected == 2 && state.overlapRejections == 2,
                          std::string(transportName(transport)) + " performs two sequential SNode.C HTTP upgrades using codex");
        result.expectTrue(state.authenticationPrepared == 2 && state.authenticationAttempts == 2 && state.authenticatedPeer &&
                              state.authenticatedPeer->encrypted == Encrypted && !state.authenticatedPeer->localPeer,
                          std::string(transportName(transport)) + " reauthenticates both physical connections as the same remote profile");
        result.expectTrue(state.welcomeReceived == 2 && state.snapshotReceived == 1 && state.syncCompleteReceived == 2 &&
                              state.synchronized == 2 && state.readyTransitions == 2 && state.synchronizedWhileConnected &&
                              state.unintentionalDisconnects == 1 && state.localShutdowns == 1 && state.postReconnectCommands == 1 &&
                              state.sessionIds.size() == 2 && state.sessionIds.front() != state.sessionIds.back(),
                          std::string(transportName(transport)) +
                              " survives one unintentional loss, reuses one SDK Client, creates a new observer session, and completes one "
                              "explicit command after reconnect");
        if (transport == Transport::WebSocketIpv4) {
            result.expectEqual(
                -SIGINT, eventLoopResult, "the production WebSocket subprotocol classifies SIGINT as intentional before transport detach");
        }
        return result.processResult();
    }

    int run(int argc, char* argv[], tests::support::TestResult& result, Transport transport) {
        if (usesIpv6(transport) && !ipv6LoopbackAvailable(result)) {
            return result.processResult() == 0 ? tests::support::cTestSkipReturnCode : result.processResult();
        }
#if !defined(AISUITE_CODEX_WEBSOCKET_ACCEPTANCE_TLS)
        if (usesTls(transport)) {
            std::cout << "WSS CLI adapter loopback skipped: TLS support is not compiled.\n";
            return tests::support::cTestSkipReturnCode;
        }
#endif

        switch (transport) {
            case Transport::WebSocketIpv4:
                return runLoopback<false, express::legacy::in::WebApp, web::http::legacy::in::Client>(
                    argc, argv, result, transport, net::in::SocketAddress("127.0.0.1", 0), "127.0.0.1");
            case Transport::WebSocketIpv6:
                return runLoopback<false, express::legacy::in6::WebApp, web::http::legacy::in6::Client>(
                    argc, argv, result, transport, net::in6::SocketAddress("::1", 0), "::1");
#if defined(AISUITE_CODEX_WEBSOCKET_ACCEPTANCE_TLS)
            case Transport::WssIpv4:
                return runLoopback<true, express::tls::in::WebApp, web::http::tls::in::Client>(
                    argc, argv, result, transport, net::in::SocketAddress("127.0.0.1", 0), "127.0.0.1");
            case Transport::WssIpv6:
                return runLoopback<true, express::tls::in6::WebApp, web::http::tls::in6::Client>(
                    argc, argv, result, transport, net::in6::SocketAddress("::1", 0), "::1");
#else
            case Transport::WssIpv4:
            case Transport::WssIpv6:
                break;
#endif
        }
        result.expectTrue(false, "unhandled CLI WebSocket transport scenario");
        return result.processResult();
    }
} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    if (argc != 2) {
        result.expectTrue(false, "expected one CLI WebSocket transport scenario argument");
        return result.processResult();
    }
    const auto transport = parseTransport(argv[1]);
    if (!transport) {
        result.expectTrue(false, "unknown CLI WebSocket transport scenario: " + std::string(argv[1]));
        return result.processResult();
    }
    return run(argc, argv, result, *transport);
}
