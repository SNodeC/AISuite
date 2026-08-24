/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CommunicationTrace.h"
#include "RealAppServerFixture.h"
#include "TestHarness.h"
#include "ai/openai/codex/bridge/CodexBridge.h"
#include "ai/openai/codex/bridge/Endpoint.h"
#include "ai/openai/codex/frontend/CodexBridge.h"
#include "ai/openai/codex/frontend/client/ClientConnection.h"
#include "ai/openai/codex/frontend/client/WebSocketClient.h"
#include "ai/openai/codex/protocol/generated/ProtocolTypes.h"
#include "apps/codex-bridge/WebSocketApplication.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/socket/State.h"
#include "core/timer/Timer.h"
#include "express/legacy/in/WebApp.h"
#include "express/legacy/in6/WebApp.h"
#include "net/in/SocketAddress.h"
#include "net/in/stream/legacy/SocketClient.h"
#include "net/in6/SocketAddress.h"
#include "net/in6/stream/legacy/SocketClient.h"
#if defined(AISUITE_CODEX_TEST_WEBSOCKET_TLS)
#include "express/tls/in/WebApp.h"
#include "express/tls/in6/WebApp.h"
#include "net/in/stream/tls/SocketClient.h"
#include "net/in6/stream/tls/SocketClient.h"
#endif
#include "net/config/ConfigInstance.h"
#include "utils/Timeval.h"
#include "web/http/ConfigHttpParser.h"
#include "web/http/ConfigWebSocket.h"
#include "web/http/client/ConfigHTTP.h"
#include "web/http/client/Request.h"
#include "web/http/server/ConfigHttpServer.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {
    namespace codex = ai::openai::codex;
    namespace bridge = codex::bridge;
    namespace frontend = codex::frontend;
    namespace client = frontend::client;
    namespace v2 = codex::generated::v2;

    constexpr std::string_view Endpoint = "/codex";
    constexpr std::size_t MaximumMessageBytes = 256U * 1024U;
    constexpr std::size_t MaximumWriteQueueBytes = 512U * 1024U;
    constexpr int SkipReturnCode = 77;

    enum class Transport { WebSocketIpv4, WebSocketIpv6, WssIpv4, WssIpv6 };

    std::optional<Transport> parseTransport(std::string_view value) {
        if (value == "websocket-ipv4") {
            return Transport::WebSocketIpv4;
        }
        if (value == "websocket-ipv6") {
            return Transport::WebSocketIpv6;
        }
        if (value == "wss-ipv4") {
            return Transport::WssIpv4;
        }
        if (value == "wss-ipv6") {
            return Transport::WssIpv6;
        }
        return std::nullopt;
    }

    bool usesIpv6(Transport transport) noexcept {
        return transport == Transport::WebSocketIpv6 || transport == Transport::WssIpv6;
    }

    bool usesTls(Transport transport) noexcept {
        return transport == Transport::WssIpv4 || transport == Transport::WssIpv6;
    }

    bool expectsNotification(Transport transport) noexcept {
        return transport == Transport::WebSocketIpv4;
    }

    std::string_view name(Transport transport) noexcept {
        switch (transport) {
            case Transport::WebSocketIpv4:
                return "WebSocket IPv4";
            case Transport::WebSocketIpv6:
                return "WebSocket IPv6";
            case Transport::WssIpv4:
                return "WSS IPv4";
            case Transport::WssIpv6:
                return "WSS IPv6";
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

    void configureWebSocketPolicy(net::config::ConfigInstance* config) {
        auto* http = config->getSubCommand<web::http::server::ConfigHttpServer>();
        http->setMaximumPendingRequests(1)->setAllowChunkedTransfer(false)->setAllowPipelining(false);
        http->getParserConfig()
            ->setMaximumStartLineBytes(8192)
            ->setMaximumHeaderLineBytes(8192)
            ->setMaximumHeaderBytes(65536)
            ->setMaximumHeaderFields(128)
            ->setMaximumBodyBytes(1);
        config->getSubCommand<web::http::ConfigWebSocket>()
            ->setMaximumFrameBytes(MaximumMessageBytes)
            ->setMaximumMessageBytes(MaximumMessageBytes)
            ->setMaximumFragments(64);
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
        Scenario(tests::codex::TestHarness& test, Transport transport, bool realAppServer = false)
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
            tests::codex::traceCommunication(name(transport), "scenario", "local", "begin");
            if (realAppServer) {
                v2::ThreadStartParams params({{"cwd", workingDirectory}});
                sdk->threadStart(params, [this](v2::ThreadStartResponse& response) {
                    tests::codex::traceCommunication(
                        name(transport), "typed-callback", "provider-to-client", "thread/start", response.getRaw());
                    startedThreadId = response.thread().id();
                    test.expect(response && startedThreadId && !response.getRaw().contains("jsonrpc"),
                                std::string(name(transport)) +
                                    " unwraps the real app-server thread/start response without a jsonrpc member");
                    responseReceived = true;
                    completeIfReady();
                });
                return;
            }
            v2::ThreadListParams params({{"cursor", std::string(name(transport))}, {"limit", 4}});
            sdk->threadList(params, [this](v2::ThreadListResponse& response) {
                tests::codex::traceCommunication(
                    name(transport), "typed-callback", "provider-to-client", "thread/list", response.getRaw());
                test.expect(response && response.nextCursor() == std::optional<std::string>{std::string(name(transport)) + "-response"},
                            std::string(name(transport)) + " completes a typed thread/list callback");
                test.expect(response.getRaw().at("result").value("wire", std::string{}) == "websocket",
                            std::string(name(transport)) + " preserves the provider's raw response fields");
                responseReceived = true;
                completeIfReady();
            });
        }

        void completeIfReady() {
            const bool eventReady = realAppServer || expectsNotification(transport) ? notificationReceived : true;
            if (completed || !responseReceived || !eventReady) {
                return;
            }
            if (realAppServer) {
                test.expect(startedThreadId && startedThreadId == notificationThreadId,
                            std::string(name(transport)) +
                                " correlates the real thread/start response with the real thread/started notification");
            }
            completed = true;
            tests::codex::traceCommunication(name(transport), "scenario", "local", "complete");
            core::EventReceiver::atNextTick([this] {
                if (connection != nullptr) {
                    connection->shutdown();
                }
                if (shutdownBinding) {
                    shutdownBinding();
                }
                if (terminateClient) {
                    terminateClient();
                }
                core::SNodeC::stop();
            });
        }

        tests::codex::TestHarness& test;
        Transport transport;
        frontend::CodexBridge* sdk = nullptr;
        client::ClientConnection* connection = nullptr;
        std::function<void()> shutdownBinding;
        std::function<void()> terminateClient;
        std::size_t providerRequests = 0;
        std::size_t connected = 0;
        std::size_t disconnected = 0;
        std::size_t rawOutbound = 0;
        std::size_t rawInbound = 0;
        bool started = false;
        bool responseReceived = false;
        bool notificationReceived = false;
        bool completed = false;
        bool failed = false;
        bool timedOut = false;
        bool realAppServer = false;
        bool providerReady = false;
        std::string workingDirectory;
        std::optional<std::string> startedThreadId;
        std::optional<std::string> notificationThreadId;
    };

    bool Provider::send(const nlohmann::json& message) {
        tests::codex::traceCommunication(name(scenario_.transport), "mock-app-server", "bridge-to-provider", "request", message);
        if (message.value("method", std::string{}) != "thread/list" || !message.contains("id")) {
            scenario_.fail(std::string(name(scenario_.transport)) + " provider received an unexpected message");
            return false;
        }
        ++scenario_.providerRequests;
        const nlohmann::json request = message;
        core::EventReceiver::atNextTick([this, request] {
            const nlohmann::json response{{"jsonrpc", "2.0"},
                                          {"id", request.at("id")},
                                          {"result",
                                           {{"data", nlohmann::json::array()},
                                            {"nextCursor", request.at("params").value("cursor", std::string{}) + "-response"},
                                            {"wire", "websocket"}}}};
            tests::codex::traceCommunication(name(scenario_.transport), "mock-app-server", "provider-to-bridge", "response", response);
            router_.receiveFromAppServer(response);
            if (expectsNotification(scenario_.transport)) {
                const nlohmann::json notification{{"jsonrpc", "2.0"},
                                                  {"method", "thread/name/updated"},
                                                  {"params", {{"threadId", "websocket-thread"}, {"threadName", "upgraded"}}}};
                tests::codex::traceCommunication(
                    name(scenario_.transport), "mock-app-server", "provider-to-bridge", "notification", notification);
                router_.receiveFromAppServer(notification);
            }
        });
        return true;
    }

    template <bool Encrypted, typename WebApp, typename HttpClient, typename Address>
    int runLoopback(char* argv[],
                    tests::codex::TestHarness& test,
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
            std::unique_ptr<tests::codex::RealAppServerFixture> realProvider;
            if (realAppServerExecutable.empty()) {
                router.setAppServer(&provider);
                router.appServerConnected();
                router.setAppServerReady();
                scenario.providerReady = true;
            } else {
                realProvider = std::make_unique<tests::codex::RealAppServerFixture>(
                    router,
                    realAppServerExecutable,
                    std::string(name(transport)),
                    [&scenario] {
                        scenario.providerReady = true;
                        scenario.begin();
                    },
                    [&scenario](std::string reason) { scenario.fail(std::move(reason)); });
                scenario.workingDirectory = realProvider->codexHome();
                test.expect(realProvider->start(), std::string(name(transport)) + " starts a real Codex app-server");
            }

            frontend::CodexBridge sdk({});
            scenario.sdk = &sdk;
            sdk.onRawJson([&scenario](codex::protocol::AppServerDirection direction, const nlohmann::json& message) {
                tests::codex::traceCommunication(name(scenario.transport),
                                                  "frontend-sdk",
                                                  direction == codex::protocol::AppServerDirection::ToAppServer ? "client-to-bridge"
                                                                                                                 : "bridge-to-client",
                                                  "raw-app-server-json",
                                                  message);
                direction == codex::protocol::AppServerDirection::ToAppServer ? ++scenario.rawOutbound : ++scenario.rawInbound;
            });
            sdk.onBridgeEvent([&scenario](const nlohmann::json& event) {
                tests::codex::traceCommunication(name(scenario.transport), "frontend-sdk", "bridge-to-client", "bridge-telemetry", event);
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
                scenario.test.expect(notification.threadId() == std::optional<std::string>{"websocket-thread"} &&
                                         notification.threadName() == std::optional<std::string>{"upgraded"},
                                     "WebSocket IPv4 delivers typed provider notification after HTTP upgrade");
                scenario.notificationReceived = true;
                scenario.completeIfReady();
            });
            sdk.onThreadStarted([&scenario](v2::ThreadStartedNotification& notification) {
                if (!scenario.realAppServer) {
                    return;
                }
                tests::codex::traceCommunication(
                    name(scenario.transport), "typed-handler", "provider-to-client", "thread/started", notification.getRaw());
                scenario.notificationThreadId = notification.thread().id();
                scenario.test.expect(scenario.notificationThreadId && !notification.getRaw().contains("jsonrpc"),
                                     std::string(name(scenario.transport)) +
                                         " unwraps the real app-server thread/started notification without a jsonrpc member");
                scenario.notificationReceived = true;
                scenario.completeIfReady();
            });

            client::ClientConnection connection(
                sdk,
                {.onConnected =
                     [&scenario] {
                         tests::codex::traceCommunication(name(scenario.transport), "websocket-binding", "lifecycle", "connected");
                         ++scenario.connected;
                     },
                 .onDisconnected =
                     [&scenario] {
                         tests::codex::traceCommunication(name(scenario.transport), "websocket-binding", "lifecycle", "disconnected");
                         ++scenario.disconnected;
                     },
                 .onFailure =
                     [&scenario](std::string reason) {
                         scenario.fail(std::string(name(scenario.transport)) + " client failed: " + reason);
                     }});
            scenario.connection = &connection;

            WebApp webApp("codex-websocket-transport-server");
            webApp.getConfig()->Instance::forceUnrequired();
            webApp.getConfig()->Connection::setMaximumWriteQueueBytes(MaximumWriteQueueBytes);
            configureWebSocketPolicy(webApp.getConfig());
            apps::codex_bridge::configureWebSocketApplication(webApp, router, std::string(Endpoint), MaximumMessageBytes);

            client::linkWebSocketClient();
            auto binding = std::make_shared<client::WebSocketBinding>(connection, MaximumMessageBytes);
            const auto beginUpgrade = [binding, &scenario](const std::shared_ptr<web::http::client::MasterRequest>& request) {
                tests::codex::traceCommunication(
                    name(scenario.transport), "http-client", "client-to-server", "websocket-upgrade-begin", {{"endpoint", Endpoint}});
                binding->beginUpgrade(request, std::string(Endpoint));
            };
            const auto endHttp = [binding](const std::shared_ptr<web::http::client::MasterRequest>& request) {
                binding->httpDisconnected(request);
            };
            HttpClient httpClient("codex-websocket-transport-client", beginUpgrade, endHttp, binding);
            httpClient.getConfig()->Instance::forceUnrequired();
            httpClient.getConfig()->setRetry(false);
            webApp.getConfig()->Connection::setReadTimeout(utils::Timeval({0, 0}));
            httpClient.getConfig()->Connection::setReadTimeout(utils::Timeval({0, 0}));
            httpClient.getConfig()->Connection::setMaximumWriteQueueBytes(MaximumWriteQueueBytes);
            scenario.shutdownBinding = [binding] {
                binding->shutdown();
            };
            scenario.terminateClient = [&httpClient] {
                static_cast<void>(httpClient.getFlowController()->terminateFlow());
            };

            if constexpr (Encrypted) {
#if defined(AISUITE_CODEX_TEST_WEBSOCKET_TLS)
                webApp.getConfig()->Tls::setCert(certificate);
                webApp.getConfig()->Tls::setCertKey(key);
                httpClient.getConfig()->Tls::setCaCert(certificate);
                httpClient.getConfig()->Tls::setCaCertAcceptUnknown(false);
                httpClient.getConfig()->Tls::setSni("localhost");
#endif
            } else {
                static_cast<void>(certificate);
                static_cast<void>(key);
            }

            webApp.listen(listenAddress, [&](const Address& bound, core::socket::State state) {
                tests::codex::traceCommunication(name(transport),
                                                  "websocket-listener",
                                                  "lifecycle",
                                                  "listen-result",
                                                  {{"address", bound.toString()}, {"state", state.what()}});
                if (state != core::socket::State::OK || bound.getPort() == 0) {
                    scenario.fail(std::string(name(transport)) + " listener failed: " + state.what());
                    return;
                }
                if (usesIpv6(transport)) {
                    httpClient.getConfig()
                        ->net::config::ConfigInstance::template getSubCommand<web::http::client::ConfigHttpClient>()
                        ->setHostHeader("[" + connectHost + "]:" + std::to_string(bound.getPort()));
                }
                httpClient.connect(Address(connectHost, bound.getPort()), [&scenario](const Address&, core::socket::State connectState) {
                    tests::codex::traceCommunication(
                        name(scenario.transport), "http-client", "lifecycle", "connect-result", {{"state", connectState.what()}});
                    if (connectState != core::socket::State::OK) {
                        scenario.fail(std::string(name(scenario.transport)) + " HTTP connector failed: " + connectState.what());
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
            static_cast<void>(httpClient.getFlowController()->terminateFlow());
            binding->shutdown();
            connection.shutdown();
            scenario.connection = nullptr;
        }

        const std::size_t expectedInbound = scenario.realAppServer || expectsNotification(transport) ? 2U : 1U;
        test.expect(eventLoopResult == 0 && scenario.completed && !scenario.failed && !scenario.timedOut,
                    std::string(name(transport)) + " completes its bounded production-shape lifecycle");
        test.expect(scenario.connected == 1 && (scenario.realAppServer || scenario.providerRequests == 1) && scenario.responseReceived,
                    std::string(name(transport)) + " upgrades once and completes its asynchronous SDK callback");
        test.expect(scenario.rawOutbound == 1 &&
                        (scenario.realAppServer ? scenario.rawInbound >= expectedInbound : scenario.rawInbound == expectedInbound),
                    std::string(name(transport)) + " preserves exact app-server message directions over WebSocket");
        return test.result();
    }
} // namespace

int main(int argc, char* argv[]) {
    tests::codex::TestHarness test;
    if (argc < 2) {
        test.expect(false, "expected one WebSocket transport argument");
        return test.result();
    }
    const std::optional<Transport> transport = parseTransport(argv[1]);
    if (!transport) {
        test.expect(false, "unknown WebSocket transport");
        return test.result();
    }
    if (usesIpv6(*transport) && !ipv6LoopbackAvailable()) {
        std::cout << "SKIP: IPv6 loopback is unavailable\n";
        return SkipReturnCode;
    }
    if (usesTls(*transport) && argc < 4) {
        test.expect(false, "WSS transport requires generated certificate and key paths");
        return test.result();
    }
    const std::string certificate = usesTls(*transport) ? argv[2] : std::string{};
    const std::string key = usesTls(*transport) ? argv[3] : std::string{};
    std::string realAppServerExecutable;
    for (int index = 2; index + 1 < argc; ++index) {
        if (std::string_view(argv[index]) == "--real-app-server") {
            realAppServerExecutable = argv[index + 1];
        }
    }

    switch (*transport) {
        case Transport::WebSocketIpv4:
            return runLoopback<false, express::legacy::in::WebApp, client::WebSocketHttpClient<net::in::stream::legacy::SocketClient>>(
                argv, test, *transport, net::in::SocketAddress("127.0.0.1", 0), "127.0.0.1", certificate, key, realAppServerExecutable);
        case Transport::WebSocketIpv6:
            return runLoopback<false, express::legacy::in6::WebApp, client::WebSocketHttpClient<net::in6::stream::legacy::SocketClient>>(
                argv, test, *transport, net::in6::SocketAddress("::1", 0), "::1", certificate, key, realAppServerExecutable);
#if defined(AISUITE_CODEX_TEST_WEBSOCKET_TLS)
        case Transport::WssIpv4:
            return runLoopback<true, express::tls::in::WebApp, client::WebSocketHttpClient<net::in::stream::tls::SocketClient>>(
                argv, test, *transport, net::in::SocketAddress("127.0.0.1", 0), "127.0.0.1", certificate, key, realAppServerExecutable);
        case Transport::WssIpv6:
            return runLoopback<true, express::tls::in6::WebApp, client::WebSocketHttpClient<net::in6::stream::tls::SocketClient>>(
                argv, test, *transport, net::in6::SocketAddress("::1", 0), "::1", certificate, key, realAppServerExecutable);
#else
        case Transport::WssIpv4:
        case Transport::WssIpv6:
            return SkipReturnCode;
#endif
    }
    return test.result();
}
