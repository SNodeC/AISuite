/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/Messages.h"
#include "ai/openai/codex/frontend/client/Client.h"
#include "apps/codex-backend-client/ClientConnection.h"
#include "apps/codex-backend-client/CodexBackendClientSocketContextFactory.h"
#include "apps/codex-backend-client/JsonLineFramer.h"
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
#include <cstddef>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <variant>

namespace {
    namespace app = apps::codex_backend_client;
    namespace frontend = ai::openai::codex::frontend;
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
            if (clientDisconnected == 1 && serverDisconnected == 1) {
                completed = true;
                core::SNodeC::stop();
            }
        }

        tests::support::TestResult& result;
        Transport transport;
        app::ClientConnection* connection = nullptr;
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
    };

    class FakeFrontendServerContext final : public core::socket::stream::SocketContext {
    public:
        FakeFrontendServerContext(core::socket::stream::SocketConnection* connection, ScenarioState& state)
            : core::socket::stream::SocketContext(connection)
            , state(state)
            , framer(MaximumFrameBytes) {
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
            const auto* hello = std::get_if<frontend::Hello>(&decoded.value());
            if (hello == nullptr || state.helloReceived != 0) {
                state.fail("CLI adapter did not send exactly one Hello as its first JSONL object");
                close();
                return;
            }
            ++state.helloReceived;
            const auto* bearer = hello->authentication ? std::get_if<frontend::BearerCredential>(&*hello->authentication) : nullptr;
            state.result.expectTrue(bearer != nullptr && bearer->token == Bearer && !hello->resumeAfter.has_value(),
                                    "the SDK-generated remote Hello crosses the selected CLI stream adapter exactly once");

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
            const auto welcome = frame(frontend::ServerMessage{frontend::Welcome{"transport-acceptance-session",
                                                                                 frontend::SessionRole::Observer,
                                                                                 SynchronizationSequence,
                                                                                 frontend::SyncMode::Snapshot,
                                                                                 frontend::Json::object()}});
            const auto snapshot =
                frame(frontend::ServerMessage{frontend::Snapshot{SynchronizationSequence, snapshotState, frontend::Json::object()}});
            const auto complete = frame(frontend::ServerMessage{frontend::SyncComplete{SynchronizationSequence, frontend::Json::object()}});
            if (welcome && snapshot && complete) {
                std::string coalesced = *welcome + *snapshot + *complete;
                sendToPeer(coalesced.data(), coalesced.size());
            }
        }

        void onConnected() override {
            ++state.serverConnected;
        }

        void onDisconnected() override {
            ++state.serverDisconnected;
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
            if (result == app::JsonLineFramer::Result::FrameTooLarge) {
                state.fail("CLI adapter emitted an oversized JSONL frame");
                close();
            }
            return size;
        }

        bool onSignal([[maybe_unused]] int signal) override {
            return true;
        }

        ScenarioState& state;
        app::JsonLineFramer framer;
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
            sdk::ClientCallbacks{.onConnectionStateChanged =
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
                                             state.connection->disconnect();
                                         });
                                     },
                                 .onCursorAdvanced = {},
                                 .onProtocolMessage =
                                     [&state](const frontend::ServerMessage& message) {
                                         state.welcomeReceived += std::holds_alternative<frontend::Welcome>(message) ? 1U : 0U;
                                         state.snapshotReceived += std::holds_alternative<frontend::Snapshot>(message) ? 1U : 0U;
                                         state.syncCompleteReceived += std::holds_alternative<frontend::SyncComplete>(message) ? 1U : 0U;
                                     },
                                 .onDiagnostic = {}});
    }

    app::ClientConnection makeConnection(sdk::Client& client, ScenarioState& state) {
        return app::ClientConnection(
            client,
            app::ClientConnectionCallbacks{
                .onConnected =
                    [&state] {
                        ++state.clientConnected;
                    },
                .onDisconnected =
                    [&state] {
                        ++state.clientDisconnected;
                        state.stopWhenDetached();
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
            app::ClientConnection connection = makeConnection(sdk, state);
            state.connection = &connection;
            Server server("a1-7c-1-client-transport-server", state);
            Client client("a1-7c-1-client-transport-client", connection, std::size_t{MaximumFrameBytes});
            server.getConfig()->Instance::forceUnrequired();
            client.getConfig()->Instance::forceUnrequired();
#if defined(AISUITE_CODEX_FRONTEND_TLS)
            if constexpr (requires { server.getConfig()->setCert(AISUITE_CODEX_TEST_TLS_CERT); }) {
                server.getConfig()->setCert(AISUITE_CODEX_TEST_TLS_CERT);
                server.getConfig()->setCertKey(AISUITE_CODEX_TEST_TLS_KEY);
                client.getConfig()->setCaCert(AISUITE_CODEX_TEST_TLS_CERT);
                client.getConfig()->setCaCertAcceptUnknown(false);
                client.getConfig()->setSni("localhost");
            }
#endif

            server.listen(listenAddress, [&state, &client, connectHost](const Address& bound, core::socket::State status) {
                if (status != core::socket::State::OK || bound.getPort() == 0) {
                    state.fail(std::string(transportName(state.transport)) + " fake listener failed: " + status.what());
                    return;
                }
                ++state.listenerBound;
                const Address remote(connectHost, bound.getPort());
                client.connect(remote, [&state](const Address&, core::socket::State connectStatus) {
                    if (connectStatus == core::socket::State::OK) {
                        ++state.connectorSucceeded;
                    } else {
                        state.fail(std::string(transportName(state.transport)) + " CLI connector failed: " + connectStatus.what());
                    }
                });
            });

            [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
                [&state] {
                    state.timedOut = true;
                    state.fail(std::string(transportName(state.transport)) + " CLI adapter acceptance timed out");
                },
                utils::Timeval({5, 0}));
            eventLoopResult = core::SNodeC::start(utils::Timeval({7, 0}));
            state.connection = nullptr;
        }
        core::SNodeC::free();

        result.expectTrue(eventLoopResult == 0 && !state.timedOut && state.completed && state.failures == 0,
                          std::string(transportName(transport)) + " completes one deterministic SDK transport lifecycle");
        result.expectTrue(state.listenerBound == 1 && state.connectorSucceeded == 1 && state.serverConnected == 1 &&
                              state.clientConnected == 1 && state.serverDisconnected == 1 && state.clientDisconnected == 1,
                          std::string(transportName(transport)) + " uses one real SNode.C listener/client connection pair");
        result.expectTrue(state.authenticationPrepared == 1 && state.helloObservedOutbound == 1 && state.helloReceived == 1 &&
                              state.welcomeReceived == 1 && state.snapshotReceived == 1 && state.syncCompleteReceived == 1 &&
                              state.synchronized == 1 && state.readyTransitions == 1,
                          std::string(transportName(transport)) + " carries SDK Hello, Welcome, snapshot, and sync.complete exactly once");
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
