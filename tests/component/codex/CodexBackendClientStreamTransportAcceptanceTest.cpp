/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/Messages.h"
#include "ai/openai/codex/frontend/client/Client.h"
#include "ai/openai/codex/frontend/client/Controller.h"
#include "ai/openai/codex/frontend/client/Threads.h"
#include "ai/openai/codex/frontend/internal/transport/JsonLineFramer.h"
#include "ai/openai/codex/typed/Threads.h"
#include "apps/codex-backend-client/ClientConnection.h"
#include "apps/codex-backend-client/CodexBackendClientSocketContextFactory.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/socket/SocketAddress.h"
#include "core/socket/State.h"
#include "core/socket/stream/SocketConnection.h"
#include "core/socket/stream/SocketContext.h"
#include "core/socket/stream/SocketContextFactory.h"
#include "core/timer/Timer.h"
#include "net/in/SocketAddress.h"
#include "net/in/stream/legacy/SocketClient.h"
#include "net/in/stream/legacy/SocketServer.h"
#if defined(AISUITE_CODEX_FRONTEND_TLS)
#include "net/in/stream/tls/SocketClient.h"
#include "net/in/stream/tls/SocketServer.h"
#endif
#include "net/in6/SocketAddress.h"
#include "net/in6/stream/legacy/SocketClient.h"
#include "net/in6/stream/legacy/SocketServer.h"
#if defined(AISUITE_CODEX_FRONTEND_TLS)
#include "net/in6/stream/tls/SocketClient.h"
#include "net/in6/stream/tls/SocketServer.h"
#endif
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <functional>
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
    namespace app = apps::codex_backend_client;
    namespace frontend = ai::openai::codex::frontend;
    namespace jsonl = ai::openai::codex::frontend::internal::transport;
    namespace sdk = ai::openai::codex::frontend::client;

    constexpr std::size_t MaximumFrameBytes = 64U * 1024U;
    constexpr frontend::SequenceNumber SynchronizationSequence{23};
    constexpr std::string_view Bearer = "a1-7c-1-client-transport-acceptance";

    enum class Transport { Ipv4, Ipv6, TlsIpv4, TlsIpv6 };

    std::optional<Transport> parseTransport(std::string_view name) {
        if (name == "ipv4") {
            return Transport::Ipv4;
        }
        if (name == "ipv6") {
            return Transport::Ipv6;
        }
        if (name == "tls-ipv4") {
            return Transport::TlsIpv4;
        }
        if (name == "tls-ipv6") {
            return Transport::TlsIpv6;
        }
        return std::nullopt;
    }

    bool usesIpv6(Transport transport) noexcept {
        return transport == Transport::Ipv6 || transport == Transport::TlsIpv6;
    }

    [[maybe_unused]] bool usesTls(Transport transport) noexcept {
        return transport == Transport::TlsIpv4 || transport == Transport::TlsIpv6;
    }

    std::string_view transportName(Transport transport) noexcept {
        switch (transport) {
            case Transport::Ipv4:
                return "IPv4 JSONL";
            case Transport::Ipv6:
                return "IPv6 JSONL";
            case Transport::TlsIpv4:
                return "IPv4 TLS JSONL";
            case Transport::TlsIpv6:
                return "IPv6 TLS JSONL";
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
                std::cout << "IPv6 CLI adapter loopback skipped: SNode.C AI_ADDRCONFIG cannot resolve ::1 in this environment.\n";
                return false;
            }
            result.expectTrue(false, std::string("unexpected IPv6 resolver failure: ") + error.what());
            return false;
        }

        const int descriptor = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (descriptor < 0) {
            if (expectedIpv6SocketAbsence(errno)) {
                std::cout << "IPv6 CLI adapter loopback skipped: this platform has no usable IPv6 stream socket.\n";
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
            std::cout << "IPv6 CLI adapter loopback skipped: this platform cannot bind ::1.\n";
            return false;
        }
        result.expectTrue(false, "unexpected IPv6 bind probe failure");
        return false;
    }

    class FakeFrontendServerContext;

    struct ScenarioState {
        explicit ScenarioState(tests::support::TestResult& result, Transport transport)
            : result(result)
            , transport(transport) {
        }

        void fail(std::string message) {
            ++failures;
            result.expectTrue(false, std::move(message));
            core::SNodeC::stop();
        }

        void stopWhenDetached() {
            if (clientDisconnected == 2 && serverDisconnected == 2) {
                completed = true;
                core::SNodeC::stop();
            }
        }

        tests::support::TestResult& result;
        Transport transport;
        sdk::Client* sdkClient = nullptr;
        app::ClientConnection* connection = nullptr;
        FakeFrontendServerContext* serverContext = nullptr;
        bool completed = false;
        bool timedOut = false;
        std::size_t failures = 0;
        std::size_t listenerBound = 0;
        std::size_t connectorSucceeded = 0;
        std::size_t serverConnected = 0;
        std::size_t serverDisconnected = 0;
        std::size_t clientConnected = 0;
        std::size_t clientDisconnected = 0;
        std::size_t authenticationPrepared = 0;
        std::size_t helloReceived = 0;
        std::size_t helloObservedOutbound = 0;
        std::size_t welcomeReceived = 0;
        std::size_t snapshotReceived = 0;
        std::size_t syncCompleteReceived = 0;
        std::size_t synchronized = 0;
        std::size_t readyTransitions = 0;
        std::size_t localShutdowns = 0;
        std::size_t postReconnectCommands = 0;
        std::size_t postReconnectCompletions = 0;
        std::size_t explicitConnectCalls = 0;
        std::size_t explicitFlowTerminations = 0;
        std::uint64_t automaticReconnects = 0;
        const void* configuredClientIdentity = nullptr;
        bool reusedConfiguredClient = true;
        std::vector<std::string> sessionIds;
    };

    class FakeFrontendServerContext final : public core::socket::stream::SocketContext {
    public:
        FakeFrontendServerContext(core::socket::stream::SocketConnection* connection, ScenarioState& state)
            : core::socket::stream::SocketContext(connection)
            , state(state)
            , framer(MaximumFrameBytes) {
        }

        void dropPeer() {
            close();
        }

    private:
        std::optional<std::string> frame(const frontend::ServerMessage& message) {
            const auto encoded = frontend::Codec::serializeServer(message);
            if (!encoded) {
                state.fail("fake frontend server could not serialize its synchronization message");
                return std::nullopt;
            }
            std::string result = encoded.value();
            result.push_back('\n');
            return result;
        }

        void clientMessage(std::string encoded) {
            const auto decoded = frontend::Codec::decodeClient(std::string_view(encoded));
            if (!decoded) {
                state.fail("fake frontend server could not decode the CLI adapter frame: " + decoded.error().message);
                close();
                return;
            }
            if (const auto* hello = std::get_if<frontend::Hello>(&decoded.value())) {
                ++state.helloReceived;
                if (state.helloReceived > 2) {
                    state.fail("CLI adapter sent more than one Hello per physical JSONL attachment");
                    close();
                    return;
                }
                const auto* bearer = hello->authentication ? std::get_if<frontend::BearerCredential>(&*hello->authentication) : nullptr;
                const bool initial = state.helloReceived == 1;
                state.result.expectTrue(
                    bearer != nullptr && bearer->token == Bearer &&
                        (initial ? !hello->resumeAfter.has_value()
                                 : hello->resumeAfter == std::optional<frontend::SequenceNumber>{SynchronizationSequence}),
                    initial ? "the initial SDK Hello omits a replay cursor"
                            : "the second physical attachment reuses the SDK Client and resumes at its retained cursor");

                const frontend::Json snapshotState{
                    {"backendRevision", std::uint64_t{1}},
                    {"lifecycle", "ready"},
                    {"diagnostics", {{"received", std::uint64_t{0}}, {"recent", frontend::Json::array()}}},
                    {"sessions", frontend::Json::array()},
                    {"threadList", {{"hasLoadedPage", false}, {"complete", true}, {"pagesLoaded", std::uint64_t{0}}}},
                    {"threads", frontend::Json::array()},
                    {"pendingRequests", frontend::Json::array()},
                    {"codexExtensions", frontend::Json::array()},
                    {"omittedCodexExtensions", std::uint64_t{0}},
                    {"journal", {{"oldestReplayableAfter", std::uint64_t{0}}, {"currentSequence", SynchronizationSequence.value()}}},
                    {"sequenceExhausted", false}};
                const std::string sessionId = initial ? "transport-acceptance-session-1" : "transport-acceptance-session-2";
                const std::vector<frontend::FrontendMethod> methods{"controller.acquire", "thread.list"};
                const frontend::Json welcomeExtensions{{"permittedScopes", frontend::Json::array({"control", "observe"})}};
                const auto welcome =
                    frame(frontend::ServerMessage{frontend::Welcome{sessionId,
                                                                    frontend::SessionRole::Observer,
                                                                    SynchronizationSequence,
                                                                    initial ? frontend::SyncMode::Snapshot : frontend::SyncMode::Replay,
                                                                    welcomeExtensions,
                                                                    std::nullopt,
                                                                    methods,
                                                                    methods}});
                const auto snapshot = initial ? frame(frontend::ServerMessage{
                                                    frontend::Snapshot{SynchronizationSequence, snapshotState, frontend::Json::object()}})
                                              : std::optional<std::string>{std::string{}};
                const auto complete =
                    frame(frontend::ServerMessage{frontend::SyncComplete{SynchronizationSequence, frontend::Json::object()}});
                if (welcome && snapshot && complete) {
                    std::string coalesced = *welcome + *snapshot + *complete;
                    sendToPeer(coalesced.data(), coalesced.size());
                }
                return;
            }

            const auto* command = std::get_if<frontend::Command>(&decoded.value());
            if (command == nullptr) {
                state.fail("CLI adapter emitted an unexpected client-message variant after reconnect");
                close();
                return;
            }
            if (state.synchronized != 2) {
                state.fail("CLI adapter emitted a command before the reconnected session synchronized");
                close();
                return;
            }
            ++state.postReconnectCommands;
            const auto response = frame(frontend::ServerMessage{
                frontend::Response::success(command->requestId, frontend::Json{{"threads", frontend::Json::array()}})});
            if (response) {
                sendToPeer(response->data(), response->size());
            }
        }

        void onConnected() override {
            ++state.serverConnected;
            state.serverContext = this;
        }

        void onDisconnected() override {
            ++state.serverDisconnected;
            if (state.serverContext == this) {
                state.serverContext = nullptr;
            }
            state.stopWhenDetached();
        }

        std::size_t onReceivedFromPeer() override {
            std::array<char, 16U * 1024U> bytes{};
            const std::size_t size = readFromPeer(bytes.data(), bytes.size());
            if (size == 0) {
                return 0;
            }
            const auto result = framer.push(std::string_view(bytes.data(), size), [this](std::string message) {
                clientMessage(std::move(message));
            });
            if (result == jsonl::JsonLineFramer::Result::FrameTooLarge) {
                state.fail("CLI adapter emitted an oversized JSONL frame");
                close();
            }
            return size;
        }

        bool onSignal([[maybe_unused]] int signal) override {
            return true;
        }

        ScenarioState& state;
        jsonl::JsonLineFramer framer{app::DEFAULT_MAXIMUM_FRAME_SIZE};
    };

    class FakeFrontendServerFactory final : public core::socket::stream::SocketContextFactory {
    public:
        explicit FakeFrontendServerFactory(ScenarioState& state)
            : state(state) {
        }

        core::socket::stream::SocketContext* create(core::socket::stream::SocketConnection* connection) override {
            return new FakeFrontendServerContext(connection, state);
        }

    private:
        ScenarioState& state;
    };

    sdk::Client makeSdk(ScenarioState& state) {
        sdk::ClientOptions options;
        options.credentialProvider = [] {
            return sdk::AuthenticationContext{frontend::BearerCredential{std::string(Bearer)}, "transport-profile:test"};
        };
        return sdk::Client(
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
                        core::EventReceiver::atNextTick([&state] {
                            if (state.connection == nullptr || !state.connection->connected()) {
                                state.fail("CLI stream adapter detached before synchronized work could interleave");
                                return;
                            }
                            if (state.synchronized == 1) {
                                if (state.serverContext == nullptr) {
                                    state.fail("the first real JSONL server attachment disappeared before forced loss");
                                    return;
                                }
                                state.serverContext->dropPeer();
                                return;
                            }
                            if (state.synchronized != 2 || state.sdkClient == nullptr || !state.sdkClient->isReady() ||
                                state.sdkClient->controller().ownedByThisClient() || !state.sdkClient->session() ||
                                state.sdkClient->session()->role != frontend::SessionRole::Observer) {
                                state.fail("the second physical JSONL attachment did not reach a fresh observer Ready session");
                                return;
                            }
                            const sdk::Submission submission = state.sdkClient->threads().list(
                                ai::openai::codex::typed::ThreadListParams{},
                                [&state](const sdk::OperationResult<sdk::ThreadListResult>& operation) {
                                    ++state.postReconnectCompletions;
                                    state.result.expectTrue(operation && operation.value->threads.empty(),
                                                            "a typed command succeeds once after explicit reconnect");
                                    core::EventReceiver::atNextTick([&state] {
                                        if (state.transport == Transport::Ipv4) {
                                            static_cast<void>(::kill(::getpid(), SIGINT));
                                        } else if (state.connection != nullptr) {
                                            state.connection->shutdown();
                                        }
                                    });
                                });
                            state.result.expectTrue(submission.accepted(), "the reconnected Ready session accepts a new typed command");
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
    }

    app::ClientConnection makeConnection(sdk::Client& client, ScenarioState& state, std::function<void()> onDisconnected) {
        return app::ClientConnection(
            client,
            app::ClientConnectionCallbacks{
                .onConnected =
                    [&state] {
                        ++state.clientConnected;
                    },
                .onDisconnected =
                    [&state, onDisconnected = std::move(onDisconnected)] {
                        ++state.clientDisconnected;
                        onDisconnected();
                    },
                .onFailure =
                    [&state](std::string message) {
                        state.fail(std::string(transportName(state.transport)) + " CLI adapter failed: " + message);
                    },
                .onOutbound =
                    [&state](const sdk::OutboundMessage& message) {
                        if (message.kind == sdk::OutboundKind::Hello) {
                            ++state.helloObservedOutbound;
                            state.result.expectTrue(message.sensitive && message.compactJson.find('\n') == std::string::npos,
                                                    "the SDK hands one compact sensitive Hello to the JSONL adapter");
                        }
                    },
                .verifiedLocalUnix = false,
                .onBeforeTransportConnected =
                    [&state](bool localUnix) {
                        ++state.authenticationPrepared;
                        state.result.expectTrue(!localUnix, "all IP CLI adapters use remote authentication preparation");
                    },
                .onLocalShutdown =
                    [&state] {
                        ++state.localShutdowns;
                    }});
    }

    template <typename Server, typename Client, typename Address>
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
            sdk::Client sdk = makeSdk(state);
            state.sdkClient = &sdk;
            std::function<void()> disconnected;
            std::function<void()> startAttempt;
            app::ClientConnection connection = makeConnection(sdk, state, [&disconnected] {
                if (disconnected) {
                    disconnected();
                }
            });
            state.connection = &connection;
            Client configuredClient("a1-7c-1-client-transport-client", connection, std::size_t{MaximumFrameBytes});
            configuredClient.getConfig()->Instance::forceUnrequired();
            configuredClient.getConfig()->setRetry(false);
            configuredClient.getConfig()->setReconnect(true)->setReconnectTime(30);
#if defined(AISUITE_CODEX_FRONTEND_TLS)
            if constexpr (requires { configuredClient.getConfig()->setCaCert(AISUITE_CODEX_TEST_TLS_CERT); }) {
                configuredClient.getConfig()->setCaCert(AISUITE_CODEX_TEST_TLS_CERT);
                configuredClient.getConfig()->setCaCertAcceptUnknown(false);
                configuredClient.getConfig()->setSni("localhost");
            }
#endif
            disconnected = [&state, &configuredClient, &startAttempt] {
                if (state.clientDisconnected == 1) {
                    core::EventReceiver::atNextTick([&state, &configuredClient, &startAttempt] {
                        const bool terminated = configuredClient.getFlowController()->terminateFlow();
                        state.result.expectTrue(terminated,
                                                "explicit reconnect terminates the configured client's pending automatic reconnect flow");
                        state.explicitFlowTerminations += terminated ? 1U : 0U;
                        startAttempt();
                    });
                } else {
                    core::EventReceiver::atNextTick([&state, &configuredClient] {
                        static_cast<void>(configuredClient.getFlowController()->terminateFlow());
                        state.stopWhenDetached();
                    });
                }
            };
            Server server("a1-7c-1-client-transport-server", state);
            server.getConfig()->Instance::forceUnrequired();
#if defined(AISUITE_CODEX_FRONTEND_TLS)
            if constexpr (requires { server.getConfig()->setCert(AISUITE_CODEX_TEST_TLS_CERT); }) {
                server.getConfig()->setCert(AISUITE_CODEX_TEST_TLS_CERT);
                server.getConfig()->setCertKey(AISUITE_CODEX_TEST_TLS_KEY);
            }
#endif

            server.listen(listenAddress, [&](const Address& bound, core::socket::State status) {
                if (status != core::socket::State::OK || bound.getPort() == 0) {
                    state.fail(std::string(transportName(state.transport)) + " fake listener failed: " + status.what());
                    return;
                }
                ++state.listenerBound;
                const Address remote(connectHost, bound.getPort());
                startAttempt = [&, remote] {
                    const void* const identity = configuredClient.getConfig();
                    if (state.configuredClientIdentity == nullptr) {
                        state.configuredClientIdentity = identity;
                    } else if (state.configuredClientIdentity != identity) {
                        state.reusedConfiguredClient = false;
                    }
                    ++state.explicitConnectCalls;
                    configuredClient.connect(remote, [&state](const Address&, core::socket::State connectStatus) {
                        if (connectStatus == core::socket::State::OK) {
                            ++state.connectorSucceeded;
                        } else {
                            state.fail(std::string(transportName(state.transport)) + " CLI connector failed: " + connectStatus.what());
                        }
                    });
                };
                startAttempt();
            });

            [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
                [&state] {
                    state.timedOut = true;
                    state.fail(std::string(transportName(state.transport)) + " CLI adapter acceptance timed out");
                },
                utils::Timeval({5, 0}));
            eventLoopResult = core::SNodeC::start(utils::Timeval({7, 0}));
            state.automaticReconnects = configuredClient.getFlowController()->getReconnectCount();
            state.connection = nullptr;
            state.sdkClient = nullptr;
        }
        core::SNodeC::free();

        const int expectedLoopResult = transport == Transport::Ipv4 ? -SIGINT : 0;
        result.expectTrue(eventLoopResult == expectedLoopResult && !state.timedOut && state.completed && state.failures == 0,
                          std::string(transportName(transport)) + " completes two deterministic SDK transport lifecycles");
        result.expectTrue(state.listenerBound == 1 && state.connectorSucceeded == 2 && state.serverConnected == 2 &&
                              state.clientConnected == 2 && state.serverDisconnected == 2 && state.clientDisconnected == 2 &&
                              state.explicitConnectCalls == 2 && state.explicitFlowTerminations == 1 && state.automaticReconnects == 0 &&
                              state.reusedConfiguredClient,
                          std::string(transportName(transport)) +
                              " reuses one configured SNode.C client, cancels pending automatic reconnect, and starts two explicit cycles");
        result.expectTrue(state.authenticationPrepared == 2 && state.helloObservedOutbound == 2 && state.helloReceived == 2 &&
                              state.welcomeReceived == 2 && state.snapshotReceived == 1 && state.syncCompleteReceived == 2 &&
                              state.synchronized == 2 && state.readyTransitions == 2 && state.localShutdowns == 1 &&
                              state.postReconnectCommands == 1 && state.postReconnectCompletions == 1 &&
                              state.sessionIds ==
                                  std::vector<std::string>{"transport-acceptance-session-1", "transport-acceptance-session-2"},
                          std::string(transportName(transport)) +
                              " reuses one SDK Client, resumes once, stays observer, and completes one new command exactly once");
        if (transport == Transport::Ipv4) {
            result.expectEqual(
                -SIGINT, eventLoopResult, "the production stream SocketContext classifies SIGINT as intentional before transport detach");
        }
        return result.processResult();
    }

    int run(int argc, char* argv[], tests::support::TestResult& result, Transport transport) {
        if (usesIpv6(transport) && !ipv6LoopbackAvailable(result)) {
            return result.processResult() == 0 ? tests::support::cTestSkipReturnCode : result.processResult();
        }
#if !defined(AISUITE_CODEX_FRONTEND_TLS)
        if (usesTls(transport)) {
            std::cout << "TLS CLI adapter loopback skipped: TLS support is not compiled.\n";
            return tests::support::cTestSkipReturnCode;
        }
#endif

        switch (transport) {
            case Transport::Ipv4: {
                using Server = net::in::stream::legacy::SocketServer<FakeFrontendServerFactory, ScenarioState&>;
                using Client =
                    net::in::stream::legacy::SocketClient<app::CodexBackendClientSocketContextFactory, app::ClientConnection&, std::size_t>;
                return runLoopback<Server, Client>(argc, argv, result, transport, net::in::SocketAddress("127.0.0.1", 0), "127.0.0.1");
            }
            case Transport::Ipv6: {
                using Server = net::in6::stream::legacy::SocketServer<FakeFrontendServerFactory, ScenarioState&>;
                using Client = net::in6::stream::legacy::
                    SocketClient<app::CodexBackendClientSocketContextFactory, app::ClientConnection&, std::size_t>;
                return runLoopback<Server, Client>(argc, argv, result, transport, net::in6::SocketAddress("::1", 0), "::1");
            }
#if defined(AISUITE_CODEX_FRONTEND_TLS)
            case Transport::TlsIpv4: {
                using Server = net::in::stream::tls::SocketServer<FakeFrontendServerFactory, ScenarioState&>;
                using Client =
                    net::in::stream::tls::SocketClient<app::CodexBackendClientSocketContextFactory, app::ClientConnection&, std::size_t>;
                return runLoopback<Server, Client>(argc, argv, result, transport, net::in::SocketAddress("127.0.0.1", 0), "127.0.0.1");
            }
            case Transport::TlsIpv6: {
                using Server = net::in6::stream::tls::SocketServer<FakeFrontendServerFactory, ScenarioState&>;
                using Client =
                    net::in6::stream::tls::SocketClient<app::CodexBackendClientSocketContextFactory, app::ClientConnection&, std::size_t>;
                return runLoopback<Server, Client>(argc, argv, result, transport, net::in6::SocketAddress("::1", 0), "::1");
            }
#else
            case Transport::TlsIpv4:
            case Transport::TlsIpv6:
                break;
#endif
        }
        result.expectTrue(false, "unhandled CLI stream transport scenario");
        return result.processResult();
    }
} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    if (argc != 2) {
        result.expectTrue(false, "expected one CLI stream transport scenario argument");
        return result.processResult();
    }
    const auto transport = parseTransport(argv[1]);
    if (!transport) {
        result.expectTrue(false, "unknown CLI stream transport scenario: " + std::string(argv[1]));
        return result.processResult();
    }
    return run(argc, argv, result, *transport);
}
