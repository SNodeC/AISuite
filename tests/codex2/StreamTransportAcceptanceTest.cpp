/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CommunicationTrace.h"
#include "RealAppServerFixture.h"
#include "TestHarness.h"
#include "ai/openai/codex2/bridge/CodexBridge.h"
#include "ai/openai/codex2/bridge/Endpoint.h"
#include "ai/openai/codex2/frontend/CodexBridge.h"
#include "ai/openai/codex2/frontend/StreamSocketContextFactory.h"
#include "ai/openai/codex2/frontend/client/ClientConnection.h"
#include "ai/openai/codex2/frontend/client/StreamSocketContextFactory.h"
#include "ai/openai/codex2/protocol/generated/ProtocolTypes.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/socket/SocketAddress.h"
#include "core/socket/State.h"
#include "core/timer/Timer.h"
#include "net/in/SocketAddress.h"
#include "net/in/stream/legacy/SocketClient.h"
#include "net/in/stream/legacy/SocketServer.h"
#include "net/in6/SocketAddress.h"
#include "net/in6/stream/legacy/SocketClient.h"
#include "net/in6/stream/legacy/SocketServer.h"
#include "net/un/SocketAddress.h"
#include "net/un/stream/legacy/SocketClient.h"
#include "net/un/stream/legacy/SocketServer.h"
#if defined(AISUITE_CODEX2_TEST_TLS)
#include "net/in/stream/tls/SocketClient.h"
#include "net/in/stream/tls/SocketServer.h"
#include "net/in6/stream/tls/SocketClient.h"
#include "net/in6/stream/tls/SocketServer.h"
#endif
#include "utils/Timeval.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
    namespace codex2 = ai::openai::codex2;
    namespace bridge = codex2::bridge;
    namespace frontend = codex2::frontend;
    namespace client = frontend::client;
    namespace v2 = codex2::generated::v2;

    constexpr std::size_t MaximumFrameBytes = 256U * 1024U;
    constexpr std::size_t MaximumWriteQueueBytes = 512U * 1024U;
    constexpr int SkipReturnCode = 77;

    enum class Transport { Unix, Ipv4, Ipv6, TlsIpv4, TlsIpv6 };

    std::optional<Transport> parseTransport(std::string_view value) {
        if (value == "unix") {
            return Transport::Unix;
        }
        if (value == "ipv4") {
            return Transport::Ipv4;
        }
        if (value == "ipv6") {
            return Transport::Ipv6;
        }
        if (value == "tls-ipv4") {
            return Transport::TlsIpv4;
        }
        if (value == "tls-ipv6") {
            return Transport::TlsIpv6;
        }
        return std::nullopt;
    }

    bool usesIpv6(Transport transport) noexcept {
        return transport == Transport::Ipv6 || transport == Transport::TlsIpv6;
    }

    bool usesTls(Transport transport) noexcept {
        return transport == Transport::TlsIpv4 || transport == Transport::TlsIpv6;
    }

    std::string_view name(Transport transport) noexcept {
        switch (transport) {
            case Transport::Unix:
                return "Unix JSONL";
            case Transport::Ipv4:
                return "IPv4 JSONL";
            case Transport::Ipv6:
                return "IPv6 JSONL";
            case Transport::TlsIpv4:
                return "TLS IPv4 JSONL";
            case Transport::TlsIpv6:
                return "TLS IPv6 JSONL";
        }
        return "unknown";
    }

    bool ipv6LoopbackAvailable() {
        const int descriptor = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (descriptor < 0) {
            return false;
        }
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_addr = in6addr_loopback;
        const bool available = ::bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
        static_cast<void>(::close(descriptor));
        return available;
    }

    struct Scenario;

    class Provider final : public bridge::AppServerEndpoint {
    public:
        Provider(bridge::CodexBridge& router, Scenario& scenario)
            : router_(router)
            , scenario_(scenario) {
        }

        bool send(const nlohmann::json& message) override;

        bool isConnected() const noexcept override {
            return true;
        }

    private:
        bridge::CodexBridge& router_;
        Scenario& scenario_;
    };

    struct Scenario {
        Scenario(tests::codex2::TestHarness& test, Transport transport, bool realAppServer = false)
            : test(test)
            , transport(transport)
            , realAppServer(realAppServer) {
        }

        void fail(std::string reason) {
            test.expect(false, reason);
            failed = true;
            core::SNodeC::stop();
        }

        void begin() {
            if (started || sdk == nullptr || !sdk->connectionId() || !providerReady) {
                return;
            }
            started = true;
            tests::codex2::traceCommunication(name(transport), "scenario", "local", "begin");
            if (realAppServer) {
                v2::ThreadStartParams params({{"cwd", workingDirectory}});
                expectedResponses = 1;
                sdk->threadStart(params, [this](v2::ThreadStartResponse& response) {
                    tests::codex2::traceCommunication(
                        name(transport), "typed-callback", "provider-to-client", "thread/start", response.getRaw());
                    startedThreadId = response.thread().id();
                    test.expect(response && startedThreadId && !response.getRaw().contains("jsonrpc"),
                                std::string(name(transport)) +
                                    " unwraps the real app-server thread/start response without a jsonrpc member");
                    ++responses;
                    completeIfReady();
                });
                return;
            }
            const std::size_t requests = transport == Transport::Ipv4 ? 2U : 1U;
            expectedResponses = requests;
            for (std::size_t index = 0; index < requests; ++index) {
                const std::string cursor = "request-" + std::to_string(index + 1U);
                v2::ThreadListParams params({{"cursor", cursor}, {"limit", static_cast<std::int64_t>(index + 2U)}});
                sdk->threadList(params, [this, cursor](v2::ThreadListResponse& response) {
                    tests::codex2::traceCommunication(
                        name(transport), "typed-callback", "provider-to-client", "thread/list", response.getRaw());
                    test.expect(response && response.nextCursor() == std::optional<std::string>{cursor + "-response"},
                                std::string(name(transport)) + " returns the correlated typed thread/list result");
                    test.expect(response.getRaw().contains("result") &&
                                    response.getRaw().at("result").value("unknownProviderField", std::string{}) ==
                                        std::string(transport == Transport::Ipv6 ? 32768U : 64U, 'x'),
                                std::string(name(transport)) + " preserves unknown provider fields and bounded payload content");
                    ++responses;
                    completeIfReady();
                });
            }
        }

        void completeIfReady() {
            if (completed || responses != expectedResponses || notifications != 1U) {
                return;
            }
            if (realAppServer) {
                test.expect(startedThreadId && startedThreadId == notificationThreadId,
                            std::string(name(transport)) +
                                " correlates the real thread/start response with the real thread/started notification");
            }
            completed = true;
            tests::codex2::traceCommunication(name(transport), "scenario", "local", "complete");
            core::EventReceiver::atNextTick([this] {
                if (connection != nullptr) {
                    connection->shutdown();
                }
                if (terminateClient) {
                    terminateClient();
                }
                core::SNodeC::stop();
            });
        }

        tests::codex2::TestHarness& test;
        Transport transport;
        frontend::CodexBridge* sdk = nullptr;
        client::ClientConnection* connection = nullptr;
        std::function<void()> terminateClient;
        std::size_t providerRequests = 0;
        std::size_t expectedResponses = 0;
        std::size_t responses = 0;
        std::size_t notifications = 0;
        std::size_t connected = 0;
        std::size_t disconnected = 0;
        std::size_t rawOutbound = 0;
        std::size_t rawInbound = 0;
        std::optional<std::string> startedThreadId;
        std::optional<std::string> notificationThreadId;
        std::string workingDirectory;
        bool realAppServer = false;
        bool providerReady = false;
        bool started = false;
        bool completed = false;
        bool failed = false;
        bool timedOut = false;
    };

    bool Provider::send(const nlohmann::json& message) {
        tests::codex2::traceCommunication(name(scenario_.transport), "mock-app-server", "bridge-to-provider", "request", message);
        if (message.value("method", std::string{}) != "thread/list" || !message.contains("id")) {
            scenario_.fail(std::string(name(scenario_.transport)) + " provider received an unexpected message");
            return false;
        }
        ++scenario_.providerRequests;
        const std::size_t requestOrdinal = scenario_.providerRequests;
        const nlohmann::json request = message;
        core::EventReceiver::atNextTick([this, request, requestOrdinal] {
            const std::string cursor = request.at("params").value("cursor", std::string{});
            const std::size_t payloadBytes = scenario_.transport == Transport::Ipv6 ? 32768U : 64U;
            const nlohmann::json response{{"jsonrpc", "2.0"},
                                          {"id", request.at("id")},
                                          {"result",
                                           {{"data", nlohmann::json::array()},
                                            {"nextCursor", cursor + "-response"},
                                            {"unknownProviderField", std::string(payloadBytes, 'x')}}}};
            tests::codex2::traceCommunication(name(scenario_.transport), "mock-app-server", "provider-to-bridge", "response", response);
            router_.receiveFromAppServer(response);
            if (requestOrdinal == scenario_.expectedResponses) {
                const nlohmann::json notification{
                    {"jsonrpc", "2.0"},
                    {"method", "thread/name/updated"},
                    {"params", {{"threadId", "transport-thread"}, {"threadName", std::string(name(scenario_.transport))}}}};
                tests::codex2::traceCommunication(
                    name(scenario_.transport), "mock-app-server", "provider-to-bridge", "notification", notification);
                router_.receiveFromAppServer(notification);
            }
        });
        return true;
    }

    void configureSdk(Scenario& scenario, frontend::CodexBridge& sdk) {
        scenario.sdk = &sdk;
        sdk.onRawJson([&scenario](codex2::protocol::AppServerDirection direction, const nlohmann::json& message) {
            tests::codex2::traceCommunication(name(scenario.transport),
                                              "frontend-sdk",
                                              direction == codex2::protocol::AppServerDirection::ToAppServer ? "client-to-bridge"
                                                                                                             : "bridge-to-client",
                                              "raw-app-server-json",
                                              message);
            direction == codex2::protocol::AppServerDirection::ToAppServer ? ++scenario.rawOutbound : ++scenario.rawInbound;
        });
        sdk.onBridgeEvent([&scenario](const nlohmann::json& event) {
            tests::codex2::traceCommunication(name(scenario.transport), "frontend-sdk", "bridge-to-client", "bridge-telemetry", event);
            if (event.value("kind", std::string{}) == "bridge.connection" && event.value("event", std::string{}) == "opened") {
                core::EventReceiver::atNextTick([&scenario] {
                    scenario.begin();
                });
            }
        });
        sdk.onThreadNameUpdated([&scenario](v2::ThreadNameUpdatedNotification& notification) {
            if (scenario.realAppServer) {
                return;
            }
            scenario.test.expect(notification.threadId() == std::optional<std::string>{"transport-thread"} &&
                                     notification.threadName() == std::optional<std::string>{std::string(name(scenario.transport))},
                                 std::string(name(scenario.transport)) +
                                     " delivers a typed provider notification through the production client stack");
            ++scenario.notifications;
            scenario.completeIfReady();
        });
        sdk.onThreadStarted([&scenario](v2::ThreadStartedNotification& notification) {
            if (!scenario.realAppServer) {
                return;
            }
            tests::codex2::traceCommunication(
                name(scenario.transport), "typed-handler", "provider-to-client", "thread/started", notification.getRaw());
            scenario.notificationThreadId = notification.thread().id();
            scenario.test.expect(scenario.notificationThreadId && !notification.getRaw().contains("jsonrpc"),
                                 std::string(name(scenario.transport)) +
                                     " unwraps the real app-server thread/started notification without a jsonrpc member");
            ++scenario.notifications;
            scenario.completeIfReady();
        });
    }

    void configureProvider(bridge::CodexBridge& router,
                           Provider& provider,
                           Scenario& scenario,
                           const std::string& realAppServerExecutable,
                           std::unique_ptr<tests::codex2::RealAppServerFixture>& realProvider) {
        if (realAppServerExecutable.empty()) {
            router.setAppServer(&provider);
            router.appServerConnected();
            router.setAppServerReady();
            scenario.providerReady = true;
            return;
        }
        realProvider = std::make_unique<tests::codex2::RealAppServerFixture>(
            router,
            realAppServerExecutable,
            std::string(name(scenario.transport)),
            [&scenario] {
                scenario.providerReady = true;
                scenario.begin();
            },
            [&scenario](std::string reason) { scenario.fail(std::move(reason)); });
        scenario.workingDirectory = realProvider->codexHome();
        scenario.test.expect(realProvider->start(), std::string(name(scenario.transport)) + " starts a real Codex app-server");
    }

    client::ClientConnection makeConnection(Scenario& scenario, frontend::CodexBridge& sdk) {
        return client::ClientConnection(
            sdk,
            {.onConnected =
                 [&scenario] {
                     tests::codex2::traceCommunication(name(scenario.transport), "client-connection", "lifecycle", "connected");
                     ++scenario.connected;
                 },
             .onDisconnected =
                 [&scenario] {
                     tests::codex2::traceCommunication(name(scenario.transport), "client-connection", "lifecycle", "disconnected");
                     ++scenario.disconnected;
                 },
             .onFailure =
                 [&scenario](std::string reason) {
                     scenario.fail(std::string(name(scenario.transport)) + " client transport failed: " + reason);
                 }});
    }

    template <typename Server, typename Client, typename Address>
    int runNetwork(char* argv[],
                   tests::codex2::TestHarness& test,
                   Transport transport,
                   const Address& listenAddress,
                   const std::string& connectHost,
                   const std::string& certificate,
                   const std::string& key,
                   const std::string& realAppServerExecutable) {
        core::SNodeC::init(1, argv);
        Scenario scenario(test, transport, !realAppServerExecutable.empty());
        int eventLoopResult = 1;
        {
            bridge::CodexBridge router;
            Provider provider(router, scenario);
            std::unique_ptr<tests::codex2::RealAppServerFixture> realProvider;
            configureProvider(router, provider, scenario, realAppServerExecutable, realProvider);

            frontend::CodexBridge sdk({});
            configureSdk(scenario, sdk);
            client::ClientConnection connection = makeConnection(scenario, sdk);
            scenario.connection = &connection;

            Server server("codex2-transport-server", router, std::size_t{MaximumFrameBytes});
            Client configuredClient("codex2-transport-client", connection, std::size_t{MaximumFrameBytes});
            server.getConfig()->Instance::forceUnrequired();
            configuredClient.getConfig()->Instance::forceUnrequired();
            configuredClient.getConfig()->setRetry(false);
            server.getConfig()->Connection::setMaximumWriteQueueBytes(MaximumWriteQueueBytes);
            configuredClient.getConfig()->Connection::setMaximumWriteQueueBytes(MaximumWriteQueueBytes);
            server.getConfig()->Connection::setReadTimeout(utils::Timeval({0, 0}));
            configuredClient.getConfig()->Connection::setReadTimeout(utils::Timeval({0, 0}));
#if defined(AISUITE_CODEX2_TEST_TLS)
            if constexpr (requires { server.getConfig()->setCert(certificate); }) {
                server.getConfig()->setCert(certificate);
                server.getConfig()->setCertKey(key);
                configuredClient.getConfig()->setCaCert(certificate);
                configuredClient.getConfig()->setCaCertAcceptUnknown(false);
                configuredClient.getConfig()->setSni("localhost");
            }
#else
            static_cast<void>(certificate);
            static_cast<void>(key);
#endif
            scenario.terminateClient = [&configuredClient] {
                static_cast<void>(configuredClient.getFlowController()->terminateFlow());
            };

            server.listen(listenAddress, [&](const Address& bound, core::socket::State state) {
                tests::codex2::traceCommunication(name(transport),
                                                  "server-listener",
                                                  "lifecycle",
                                                  "listen-result",
                                                  {{"address", bound.toString()}, {"state", state.what()}});
                if (state != core::socket::State::OK || bound.getPort() == 0) {
                    scenario.fail(std::string(name(transport)) + " listener failed: " + state.what());
                    return;
                }
                configuredClient.connect(
                    Address(connectHost, bound.getPort()), [&scenario](const Address&, core::socket::State connectState) {
                        tests::codex2::traceCommunication(
                            name(scenario.transport), "client-connector", "lifecycle", "connect-result", {{"state", connectState.what()}});
                        if (connectState != core::socket::State::OK) {
                            scenario.fail(std::string(name(scenario.transport)) + " connector failed: " + connectState.what());
                        }
                    });
            });

            [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
                [&scenario] {
                    scenario.timedOut = true;
                    scenario.fail(std::string(name(scenario.transport)) + " timed out");
                },
                utils::Timeval({15, 0}));
            eventLoopResult = core::SNodeC::start(utils::Timeval({17, 0}));
            static_cast<void>(configuredClient.getFlowController()->terminateFlow());
            connection.shutdown();
            scenario.connection = nullptr;
        }

        test.expect(eventLoopResult == 0 && scenario.completed && !scenario.failed && !scenario.timedOut,
                    std::string(name(transport)) + " completes its bounded production-shape lifecycle");
        test.expect(scenario.connected == 1 &&
                        (scenario.realAppServer || scenario.providerRequests == scenario.expectedResponses) &&
                        scenario.responses == scenario.expectedResponses && scenario.notifications == 1,
                    std::string(name(transport)) + " connects once and completes every expected asynchronous callback");
        test.expect(scenario.rawOutbound == scenario.expectedResponses &&
                        (scenario.realAppServer ? scenario.rawInbound >= scenario.expectedResponses + 1U
                                                : scenario.rawInbound == scenario.expectedResponses + 1U),
                    std::string(name(transport)) + " observes exact request, response, and notification directions");
        return test.result();
    }

    int runUnix(char* argv[],
                tests::codex2::TestHarness& test,
                const std::string& path,
                const std::string& realAppServerExecutable) {
        static_cast<void>(::unlink(path.c_str()));
        core::SNodeC::init(1, argv);
        Scenario scenario(test, Transport::Unix, !realAppServerExecutable.empty());
        int eventLoopResult = 1;
        {
            bridge::CodexBridge router;
            Provider provider(router, scenario);
            std::unique_ptr<tests::codex2::RealAppServerFixture> realProvider;
            configureProvider(router, provider, scenario, realAppServerExecutable, realProvider);
            frontend::CodexBridge sdk({});
            configureSdk(scenario, sdk);
            client::ClientConnection connection = makeConnection(scenario, sdk);
            scenario.connection = &connection;

            using Server = net::un::stream::legacy::SocketServer<frontend::StreamSocketContextFactory, bridge::CodexBridge&, std::size_t>;
            using Client =
                net::un::stream::legacy::SocketClient<client::StreamSocketContextFactory, client::ClientConnection&, std::size_t>;
            Server server("codex2-unix-transport-server", router, std::size_t{MaximumFrameBytes});
            Client configuredClient("codex2-unix-transport-client", connection, std::size_t{MaximumFrameBytes});
            server.getConfig()->Instance::forceUnrequired();
            configuredClient.getConfig()->Instance::forceUnrequired();
            configuredClient.getConfig()->setRetry(false);
            server.getConfig()->Connection::setMaximumWriteQueueBytes(MaximumWriteQueueBytes);
            configuredClient.getConfig()->Connection::setMaximumWriteQueueBytes(MaximumWriteQueueBytes);
            server.getConfig()->Connection::setReadTimeout(utils::Timeval({0, 0}));
            configuredClient.getConfig()->Connection::setReadTimeout(utils::Timeval({0, 0}));
            scenario.terminateClient = [&configuredClient] {
                static_cast<void>(configuredClient.getFlowController()->terminateFlow());
            };

            server.listen(path, [&](const net::un::SocketAddress&, core::socket::State state) {
                tests::codex2::traceCommunication(
                    "Unix JSONL", "server-listener", "lifecycle", "listen-result", {{"path", path}, {"state", state.what()}});
                if (state != core::socket::State::OK) {
                    scenario.fail("Unix JSONL listener failed: " + state.what());
                    return;
                }
                configuredClient.connect(path, [&scenario](const net::un::SocketAddress&, core::socket::State connectState) {
                    tests::codex2::traceCommunication(
                        "Unix JSONL", "client-connector", "lifecycle", "connect-result", {{"state", connectState.what()}});
                    if (connectState != core::socket::State::OK) {
                        scenario.fail("Unix JSONL connector failed: " + connectState.what());
                    }
                });
            });
            [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
                [&scenario] {
                    scenario.timedOut = true;
                    scenario.fail("Unix JSONL timed out");
                },
                utils::Timeval({15, 0}));
            eventLoopResult = core::SNodeC::start(utils::Timeval({17, 0}));
            static_cast<void>(configuredClient.getFlowController()->terminateFlow());
            connection.shutdown();
            scenario.connection = nullptr;
        }
        static_cast<void>(::unlink(path.c_str()));
        test.expect(eventLoopResult == 0 && scenario.completed && !scenario.failed && !scenario.timedOut,
                    "Unix JSONL completes its bounded production-shape lifecycle");
        test.expect(scenario.connected == 1 && (scenario.realAppServer || scenario.providerRequests == 1) && scenario.responses == 1 &&
                        scenario.notifications == 1 && scenario.rawOutbound == 1 &&
                        (scenario.realAppServer ? scenario.rawInbound >= 2 : scenario.rawInbound == 2),
                    "Unix JSONL carries typed request/response, raw directions, telemetry, and typed notification");
        return test.result();
    }
} // namespace

int main(int argc, char* argv[]) {
    tests::codex2::TestHarness test;
    if (argc < 3) {
        test.expect(false, "expected transport and Unix socket path arguments");
        return test.result();
    }
    const std::optional<Transport> transport = parseTransport(argv[1]);
    if (!transport) {
        test.expect(false, "unknown stream transport");
        return test.result();
    }
    if (usesIpv6(*transport) && !ipv6LoopbackAvailable()) {
        std::cout << "SKIP: IPv6 loopback is unavailable\n";
        return SkipReturnCode;
    }
    if (usesTls(*transport) && argc < 5) {
        test.expect(false, "TLS transport requires generated certificate and key paths");
        return test.result();
    }

    const std::string certificate = usesTls(*transport) ? argv[3] : std::string{};
    const std::string key = usesTls(*transport) ? argv[4] : std::string{};
    std::string realAppServerExecutable;
    for (int index = 2; index + 1 < argc; ++index) {
        if (std::string_view(argv[index]) == "--real-app-server") {
            realAppServerExecutable = argv[index + 1];
        }
    }
    switch (*transport) {
        case Transport::Unix:
            return runUnix(argv, test, argv[2], realAppServerExecutable);
        case Transport::Ipv4: {
            using Server = net::in::stream::legacy::SocketServer<frontend::StreamSocketContextFactory, bridge::CodexBridge&, std::size_t>;
            using Client =
                net::in::stream::legacy::SocketClient<client::StreamSocketContextFactory, client::ClientConnection&, std::size_t>;
            return runNetwork<Server, Client>(
                argv, test, *transport, net::in::SocketAddress("127.0.0.1", 0), "127.0.0.1", certificate, key, realAppServerExecutable);
        }
        case Transport::Ipv6: {
            using Server = net::in6::stream::legacy::SocketServer<frontend::StreamSocketContextFactory, bridge::CodexBridge&, std::size_t>;
            using Client =
                net::in6::stream::legacy::SocketClient<client::StreamSocketContextFactory, client::ClientConnection&, std::size_t>;
            return runNetwork<Server, Client>(
                argv, test, *transport, net::in6::SocketAddress("::1", 0), "::1", certificate, key, realAppServerExecutable);
        }
#if defined(AISUITE_CODEX2_TEST_TLS)
        case Transport::TlsIpv4: {
            using Server = net::in::stream::tls::SocketServer<frontend::StreamSocketContextFactory, bridge::CodexBridge&, std::size_t>;
            using Client = net::in::stream::tls::SocketClient<client::StreamSocketContextFactory, client::ClientConnection&, std::size_t>;
            return runNetwork<Server, Client>(
                argv, test, *transport, net::in::SocketAddress("127.0.0.1", 0), "127.0.0.1", certificate, key, realAppServerExecutable);
        }
        case Transport::TlsIpv6: {
            using Server = net::in6::stream::tls::SocketServer<frontend::StreamSocketContextFactory, bridge::CodexBridge&, std::size_t>;
            using Client = net::in6::stream::tls::SocketClient<client::StreamSocketContextFactory, client::ClientConnection&, std::size_t>;
            return runNetwork<Server, Client>(
                argv, test, *transport, net::in6::SocketAddress("::1", 0), "::1", certificate, key, realAppServerExecutable);
        }
#else
        case Transport::TlsIpv4:
        case Transport::TlsIpv6:
            return SkipReturnCode;
#endif
    }
    return test.result();
}
