/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CommunicationTrace.h"
#include "TestHarness.h"
#include "ai/openai/codex2/bridge/CodexBridge.h"
#include "ai/openai/codex2/protocol/generated/ProtocolTypes.h"
#include "ai/openai/codex2/provider/WebSocketAppServer.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/socket/State.h"
#include "core/timer/Timer.h"
#include "express/Request.h"
#include "express/Response.h"
#include "express/legacy/in/WebApp.h"
#include "express/legacy/in6/WebApp.h"
#include "express/legacy/un/WebApp.h"
#include "net/config/ConfigInstance.h"
#include "net/in/SocketAddress.h"
#include "net/in/stream/legacy/SocketClient.h"
#include "net/in6/SocketAddress.h"
#include "net/in6/stream/legacy/SocketClient.h"
#include "net/un/stream/legacy/SocketClient.h"
#include "utils/Timeval.h"
#include "utils/base64.h"
#include "web/http/ConfigHttpParser.h"
#include "web/http/ConfigWebSocket.h"
#include "web/http/client/ConfigHTTP.h"
#include "web/http/server/ConfigHttpServer.h"
#include "web/http/server/Request.h"
#include "web/http/server/Response.h"
#include "web/http/server/SocketContextUpgradeFactory.h"
#include "web/websocket/SocketContextUpgrade.h"
#include "web/websocket/SubProtocolContext.h"
#include "web/websocket/server/SubProtocol.h"

#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace {
    namespace codex2 = ai::openai::codex2;
    namespace bridge = codex2::bridge;
    namespace provider = codex2::provider;
    namespace v2 = codex2::generated::v2;

    constexpr std::size_t MaximumMessageBytes = 256U * 1024U;
    constexpr std::size_t MaximumWriteQueueBytes = 512U * 1024U;
    constexpr int SkipReturnCode = 77;
    constexpr std::string_view WebSocketUpgrade = "websocket";

    enum class Transport { Unix, Ipv4, Ipv6 };

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
        return std::nullopt;
    }

    std::string_view transportName(Transport transport) noexcept {
        switch (transport) {
            case Transport::Unix:
                return "Provider Unix WebSocket";
            case Transport::Ipv4:
                return "Provider IPv4 WebSocket";
            case Transport::Ipv6:
                return "Provider IPv6 WebSocket";
        }
        return "Provider unknown WebSocket";
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

    struct Scenario {
        Scenario(tests::codex2::TestHarness& test, Transport transport)
            : test(test)
            , transport(transport) {
        }

        void fail(std::string reason) {
            test.expect(false, reason);
            failed = true;
            core::SNodeC::stop();
        }

        void completeIfReady() {
            if (completed || !responseReceived || !notificationReceived) {
                return;
            }
            completed = true;
            tests::codex2::traceCommunication(transportName(transport), "scenario", "local", "complete");
            core::EventReceiver::atNextTick([this] {
                if (stopProvider) {
                    stopProvider();
                }
                if (terminateClient) {
                    terminateClient();
                }
                core::SNodeC::stop();
            });
        }

        tests::codex2::TestHarness& test;
        Transport transport;
        std::function<void()> stopProvider;
        std::function<void()> terminateClient;
        std::size_t rawOutbound = 0;
        std::size_t rawInbound = 0;
        std::size_t peerRequests = 0;
        bool providerConnected = false;
        bool providerDisconnected = false;
        bool responseReceived = false;
        bool notificationReceived = false;
        bool completed = false;
        bool failed = false;
        bool timedOut = false;
    };

    class FakeAppServerSubProtocol final : public web::websocket::server::SubProtocol {
    public:
        FakeAppServerSubProtocol(web::websocket::SubProtocolContext* context, Scenario& scenario)
            : web::websocket::server::SubProtocol(context, "codex-app-server-test-peer", 0, 3)
            , scenario_(scenario) {
        }

    private:
        void onConnected() override {
            tests::codex2::traceCommunication(transportName(scenario_.transport), "fake-app-server", "lifecycle", "websocket-connected");
        }

        void onMessageStart(int opCode) override {
            message_.clear();
            if (opCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
                scenario_.fail(std::string(transportName(scenario_.transport)) + " received a non-text app-server frame");
            }
        }

        void onMessageData(const char* chunk, std::size_t length) override {
            message_.append(chunk, length);
        }

        void onMessageEnd() override {
            try {
                const nlohmann::json request = nlohmann::json::parse(message_);
                tests::codex2::traceCommunication(
                    transportName(scenario_.transport), "fake-app-server", "bridge-to-provider", "websocket-request", request);
                if (!request.contains("id") || request.value("method", std::string{}) != "thread/list") {
                    scenario_.fail(std::string(transportName(scenario_.transport)) + " received an unexpected provider request");
                    return;
                }
                ++scenario_.peerRequests;
                const nlohmann::json response{{"jsonrpc", "2.0"},
                                              {"id", request.at("id")},
                                              {"result",
                                               {{"data", nlohmann::json::array()},
                                                {"nextCursor", "provider-websocket-response"},
                                                {"wire", std::string(transportName(scenario_.transport))}}}};
                tests::codex2::traceCommunication(
                    transportName(scenario_.transport), "fake-app-server", "provider-to-bridge", "websocket-response", response);
                sendMessage(response.dump());

                const nlohmann::json notification{
                    {"jsonrpc", "2.0"},
                    {"method", "thread/name/updated"},
                    {"params",
                     {{"threadId", "provider-websocket-thread"}, {"threadName", std::string(transportName(scenario_.transport))}}}};
                tests::codex2::traceCommunication(
                    transportName(scenario_.transport), "fake-app-server", "provider-to-bridge", "websocket-notification", notification);
                sendMessage(notification.dump());
            } catch (const nlohmann::json::exception& exception) {
                scenario_.fail(std::string(transportName(scenario_.transport)) + " JSON failure: " + exception.what());
            }
        }

        void onMessageError(std::uint16_t error) override {
            scenario_.fail(std::string(transportName(scenario_.transport)) + " WebSocket message error " + std::to_string(error));
        }

        void onDisconnected() override {
            tests::codex2::traceCommunication(transportName(scenario_.transport), "fake-app-server", "lifecycle", "websocket-disconnected");
        }

        bool onSignal(int signal) override {
            static_cast<void>(signal);
            sendClose();
            return false;
        }

        Scenario& scenario_;
        std::string message_;
    };

    class FakeServerSocketContextUpgrade final
        : public web::websocket::
              SocketContextUpgrade<web::websocket::server::SubProtocol, web::http::server::Request, web::http::server::Response> {
    private:
        using Super = web::websocket::
            SocketContextUpgrade<web::websocket::server::SubProtocol, web::http::server::Request, web::http::server::Response>;

    public:
        FakeServerSocketContextUpgrade(core::socket::stream::SocketConnection* connection,
                                       web::http::server::SocketContextUpgradeFactory* factory,
                                       Scenario& scenario)
            : Super(connection, factory, Role::SERVER) {
            subProtocol = new FakeAppServerSubProtocol(this, scenario);
        }

        ~FakeServerSocketContextUpgrade() override {
            delete subProtocol;
            subProtocol = nullptr;
        }
    };

    class FakeServerUpgradeFactory final : public web::http::server::SocketContextUpgradeFactory {
    public:
        explicit FakeServerUpgradeFactory(Scenario& scenario)
            : scenario_(scenario) {
        }

        std::string name() override {
            return std::string(WebSocketUpgrade);
        }

    private:
        web::http::SocketContextUpgrade<web::http::server::Request, web::http::server::Response>*
        create(core::socket::stream::SocketConnection* connection,
               web::http::server::Request* request,
               web::http::server::Response* response) override {
            if (request->get("Sec-WebSocket-Version") != "13") {
                checkRefCount();
                response->set("Sec-WebSocket-Version", "13");
                response->set("Connection", "close");
                response->status(426);
                return nullptr;
            }
            response->set("Upgrade", "websocket");
            response->set("Connection", "Upgrade");
            response->set("Sec-WebSocket-Accept", base64::serverWebSocketKey(request->get("sec-websocket-key")));
            response->status(101);
            return new FakeServerSocketContextUpgrade(connection, this, scenario_);
        }

        Scenario& scenario_;
    };

    Scenario* linkedScenario = nullptr;

    web::http::server::SocketContextUpgradeFactory* createFakeAppServerUpgradeFactory() {
        return new FakeServerUpgradeFactory(*linkedScenario);
    }

    void linkFakeAppServer(Scenario& scenario) {
        linkedScenario = &scenario;
        web::http::server::SocketContextUpgradeFactory::link(std::string(WebSocketUpgrade), createFakeAppServerUpgradeFactory);
    }

    template <typename WebApp>
    void configureFakeAppServer(WebApp& webApp, Scenario& scenario) {
        linkFakeAppServer(scenario);
        webApp.get("/", [&scenario](const std::shared_ptr<express::Request>& request, const std::shared_ptr<express::Response>& response) {
            tests::codex2::traceCommunication(transportName(scenario.transport),
                                              "fake-app-server",
                                              "bridge-to-provider",
                                              "http-upgrade-request",
                                              {{"url", request->url}});
            response->upgrade(request, [&scenario, response](const std::string& selected) {
                tests::codex2::traceCommunication(transportName(scenario.transport),
                                                  "fake-app-server",
                                                  "provider-to-bridge",
                                                  "http-upgrade-response",
                                                  {{"selectedProtocol", selected}});
                if (selected == WebSocketUpgrade) {
                    response->end();
                } else {
                    response->sendStatus(400);
                    scenario.fail(std::string(transportName(scenario.transport)) + " failed to select WebSocket protocol");
                }
            });
        });
    }

    void configureBackend(bridge::CodexBridge& router, Scenario& scenario) {
        router.onRawJson([&scenario](codex2::protocol::AppServerDirection direction, const nlohmann::json& message) {
            tests::codex2::traceCommunication(transportName(scenario.transport),
                                              "backend-sdk",
                                              direction == codex2::protocol::AppServerDirection::ToAppServer ? "bridge-to-provider"
                                                                                                             : "provider-to-bridge",
                                              "raw-app-server-json",
                                              message);
            direction == codex2::protocol::AppServerDirection::ToAppServer ? ++scenario.rawOutbound : ++scenario.rawInbound;
        });
        router.onThreadNameUpdated([&scenario](v2::ThreadNameUpdatedNotification& notification) {
            tests::codex2::traceCommunication(
                transportName(scenario.transport), "typed-handler", "provider-to-bridge", "thread/name/updated", notification.getRaw());
            scenario.test.expect(notification.threadId() == std::optional<std::string>{"provider-websocket-thread"} &&
                                     notification.threadName() ==
                                         std::optional<std::string>{std::string(transportName(scenario.transport))},
                                 std::string(transportName(scenario.transport)) + " notification reaches the generated typed handler");
            scenario.notificationReceived = true;
            scenario.completeIfReady();
        });
        router.onProviderLifecycle([&router, &scenario](bool connected) {
            tests::codex2::traceCommunication(
                transportName(scenario.transport), "provider-endpoint", "lifecycle", connected ? "connected" : "disconnected");
            if (!connected) {
                scenario.providerDisconnected = true;
                return;
            }
            scenario.providerConnected = true;
            v2::ThreadListParams params({{"cursor", "provider-websocket-request"}, {"limit", 3}});
            router.threadList(params, [&scenario](v2::ThreadListResponse& response) {
                tests::codex2::traceCommunication(
                    transportName(scenario.transport), "typed-callback", "provider-to-bridge", "thread/list", response.getRaw());
                scenario.test.expect(response && response.nextCursor() == std::optional<std::string>{"provider-websocket-response"} &&
                                         response.getRaw().at("result").value("wire", std::string{}) ==
                                             std::string(transportName(scenario.transport)),
                                     std::string(transportName(scenario.transport)) + " response completes the generated typed callback");
                scenario.responseReceived = true;
                scenario.completeIfReady();
            });
        });
    }

    template <typename WebApp, typename HttpClient, typename StartTransport>
    int runLoopback(char* argv[], tests::codex2::TestHarness& test, Transport transport, StartTransport startTransport) {
        core::SNodeC::init(1, argv);
        Scenario scenario(test, transport);
        int eventLoopResult = 1;
        {
            bridge::CodexBridge router;
            configureBackend(router, scenario);
            provider::WebSocketAppServer endpoint(router, MaximumMessageBytes);
            provider::linkAppServerWebSocketClient();

            WebApp webApp("codex2-provider-websocket-server");
            webApp.getConfig()->Instance::forceUnrequired();
            webApp.getConfig()->Connection::setMaximumWriteQueueBytes(MaximumWriteQueueBytes);
            configureWebSocketPolicy(webApp.getConfig());
            configureFakeAppServer(webApp, scenario);

            const auto beginUpgrade = [&endpoint, &scenario](const std::shared_ptr<web::http::client::MasterRequest>& request) {
                tests::codex2::traceCommunication(
                    transportName(scenario.transport), "provider-http-client", "bridge-to-provider", "websocket-upgrade-begin");
                endpoint.beginUpgrade(request);
            };
            const auto endHttp = [&endpoint](const std::shared_ptr<web::http::client::MasterRequest>& request) {
                endpoint.httpDisconnected(request);
            };
            HttpClient httpClient("codex2-provider-websocket-client", beginUpgrade, endHttp, endpoint.state());
            httpClient.getConfig()->Instance::forceUnrequired();
            httpClient.getConfig()->setRetry(false);
            httpClient.getConfig()->Connection::setMaximumWriteQueueBytes(MaximumWriteQueueBytes);
            scenario.stopProvider = [&endpoint] {
                endpoint.stop();
            };
            scenario.terminateClient = [&httpClient] {
                static_cast<void>(httpClient.getFlowController()->terminateFlow());
            };

            startTransport(webApp, httpClient, scenario);

            [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
                [&scenario] {
                    scenario.timedOut = true;
                    scenario.fail(std::string(transportName(scenario.transport)) + " acceptance timed out");
                },
                utils::Timeval({5, 0}));
            eventLoopResult = core::SNodeC::start(utils::Timeval({7, 0}));
            static_cast<void>(httpClient.getFlowController()->terminateFlow());
            endpoint.stop();
        }

        test.expect(eventLoopResult == 0 && scenario.completed && !scenario.failed && !scenario.timedOut,
                    std::string(transportName(transport)) + " completes its bounded production lifecycle");
        test.expect(scenario.providerConnected && scenario.peerRequests == 1 && scenario.rawOutbound == 1 && scenario.rawInbound == 2,
                    std::string(transportName(transport)) + " exposes request, response, notification, and provider lifecycle");
        return test.result();
    }
} // namespace

int main(int argc, char* argv[]) {
    tests::codex2::TestHarness test;
    if (argc < 3) {
        test.expect(false, "expected provider transport and Unix socket path arguments");
        return test.result();
    }
    const std::optional<Transport> transport = parseTransport(argv[1]);
    if (!transport) {
        test.expect(false, "unknown provider WebSocket transport");
        return test.result();
    }
    if (*transport == Transport::Ipv6 && !ipv6LoopbackAvailable()) {
        std::cout << "SKIP: IPv6 loopback is unavailable\n";
        return SkipReturnCode;
    }

    switch (*transport) {
        case Transport::Unix: {
            const std::string path = argv[2];
            static_cast<void>(::unlink(path.c_str()));
            const int result =
                runLoopback<express::legacy::un::WebApp, provider::WebSocketHttpClient<net::un::stream::legacy::SocketClient>>(
                    argv, test, *transport, [path](auto& webApp, auto& httpClient, Scenario& scenario) {
                        webApp.listen(path, [&httpClient, &scenario, path](const net::un::SocketAddress&, core::socket::State state) {
                            tests::codex2::traceCommunication(transportName(scenario.transport),
                                                              "provider-listener",
                                                              "lifecycle",
                                                              "listen-result",
                                                              {{"path", path}, {"state", state.what()}});
                            if (state != core::socket::State::OK) {
                                scenario.fail(std::string(transportName(scenario.transport)) + " listener failed: " + state.what());
                                return;
                            }
                            httpClient.connect(path, [&scenario](const net::un::SocketAddress&, core::socket::State connectState) {
                                tests::codex2::traceCommunication(transportName(scenario.transport),
                                                                  "provider-http-client",
                                                                  "lifecycle",
                                                                  "connect-result",
                                                                  {{"state", connectState.what()}});
                                if (connectState != core::socket::State::OK) {
                                    scenario.fail(std::string(transportName(scenario.transport)) +
                                                  " connector failed: " + connectState.what());
                                }
                            });
                        });
                    });
            static_cast<void>(::unlink(path.c_str()));
            return result;
        }
        case Transport::Ipv4:
            return runLoopback<express::legacy::in::WebApp, provider::WebSocketHttpClient<net::in::stream::legacy::SocketClient>>(
                argv, test, *transport, [](auto& webApp, auto& httpClient, Scenario& scenario) {
                    webApp.listen(net::in::SocketAddress("127.0.0.1", 0),
                                  [&httpClient, &scenario](const net::in::SocketAddress& bound, core::socket::State state) {
                                      tests::codex2::traceCommunication(transportName(scenario.transport),
                                                                        "provider-listener",
                                                                        "lifecycle",
                                                                        "listen-result",
                                                                        {{"address", bound.toString()}, {"state", state.what()}});
                                      if (state != core::socket::State::OK || bound.getPort() == 0) {
                                          scenario.fail(std::string(transportName(scenario.transport)) +
                                                        " listener failed: " + state.what());
                                          return;
                                      }
                                      httpClient.connect(net::in::SocketAddress("127.0.0.1", bound.getPort()),
                                                         [&scenario](const net::in::SocketAddress&, core::socket::State connectState) {
                                                             tests::codex2::traceCommunication(transportName(scenario.transport),
                                                                                               "provider-http-client",
                                                                                               "lifecycle",
                                                                                               "connect-result",
                                                                                               {{"state", connectState.what()}});
                                                             if (connectState != core::socket::State::OK) {
                                                                 scenario.fail(std::string(transportName(scenario.transport)) +
                                                                               " connector failed: " + connectState.what());
                                                             }
                                                         });
                                  });
                });
        case Transport::Ipv6:
            return runLoopback<express::legacy::in6::WebApp, provider::WebSocketHttpClient<net::in6::stream::legacy::SocketClient>>(
                argv, test, *transport, [](auto& webApp, auto& httpClient, Scenario& scenario) {
                    webApp.listen(net::in6::SocketAddress("::1", 0),
                                  [&httpClient, &scenario](const net::in6::SocketAddress& bound, core::socket::State state) {
                                      tests::codex2::traceCommunication(transportName(scenario.transport),
                                                                        "provider-listener",
                                                                        "lifecycle",
                                                                        "listen-result",
                                                                        {{"address", bound.toString()}, {"state", state.what()}});
                                      if (state != core::socket::State::OK || bound.getPort() == 0) {
                                          scenario.fail(std::string(transportName(scenario.transport)) +
                                                        " listener failed: " + state.what());
                                          return;
                                      }
                                      httpClient.getConfig()
                                          ->net::config::ConfigInstance::template getSubCommand<web::http::client::ConfigHttpClient>()
                                          ->setHostHeader("[::1]:" + std::to_string(bound.getPort()));
                                      httpClient.connect(net::in6::SocketAddress("::1", bound.getPort()),
                                                         [&scenario](const net::in6::SocketAddress&, core::socket::State connectState) {
                                                             tests::codex2::traceCommunication(transportName(scenario.transport),
                                                                                               "provider-http-client",
                                                                                               "lifecycle",
                                                                                               "connect-result",
                                                                                               {{"state", connectState.what()}});
                                                             if (connectState != core::socket::State::OK) {
                                                                 scenario.fail(std::string(transportName(scenario.transport)) +
                                                                               " connector failed: " + connectState.what());
                                                             }
                                                         });
                                  });
                });
    }
    return test.result();
}
