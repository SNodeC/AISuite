/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CodexBackendTestSupport.h"
#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/FrontendService.h"
#include "apps/codex-backend/Configuration.h"
#include "apps/codex-backend/FrontendWebApplication.h"
#include "core/SNodeC.h"
#include "core/socket/State.h"
#include "core/timer/Timer.h"
#include "express/tls/in/WebApp.h"
#include "net/in/SocketAddress.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"
#include "web/http/ConfigWebSocket.h"
#include "web/http/tls/in/Client.h"
#include "web/websocket/SubProtocolContext.h"
#include "web/websocket/SubProtocolFactory.h"
#include "web/websocket/client/SocketContextUpgradeFactory.h"
#include "web/websocket/client/SubProtocol.h"
#include "web/websocket/client/SubProtocolFactorySelector.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

    namespace app = apps::codex_backend;
    namespace backend = ai::openai::codex::backend;
    namespace frontend = ai::openai::codex::frontend;

    using FakeBackendCore = backend::BackendCore<tests::codex::FakeAppServerClient>;

    constexpr std::string_view Bearer = "a1-7b-wss-live-synthetic-token";
    constexpr std::string_view Endpoint = "/frontend";
    constexpr std::size_t MaximumInboundMessageBytes = 1024U * 1024U;
    constexpr std::size_t SaturatedApplicationWriterBytes = 6U * 1024U;

    struct IntegrationState;

    class ClientSubProtocol final : public web::websocket::client::SubProtocol {
    public:
        ClientSubProtocol(web::websocket::SubProtocolContext* context, IntegrationState& state);

    private:
        void onConnected() override;
        void onMessageStart(int opCode) override;
        void onMessageData(const char* chunk, std::size_t chunkLength) override;
        void onMessageEnd() override;
        void onMessageError(std::uint16_t error) override;
        void onDisconnected() override;
        bool onSignal(int signum) override;

        IntegrationState& state;
        std::string receivedWire;
    };

    struct IntegrationState {
        explicit IntegrationState(tests::support::TestResult& result)
            : result(result) {
        }

        void fail(std::string message) {
            ++unexpectedStates;
            result.expectTrue(false, std::move(message));
            core::SNodeC::stop();
        }

        tests::support::TestResult& result;
        std::shared_ptr<web::http::tls::in::Client> client;
        std::optional<frontend::FrontendPeerContext> authenticatedPeer;
        std::optional<std::string> sameOrigin;
        std::string allReceivedWire;
        std::size_t serverBinds = 0;
        std::size_t clientConnections = 0;
        std::size_t upgradesStarted = 0;
        std::size_t upgradesCompleted = 0;
        std::size_t websocketConnections = 0;
        std::size_t websocketDisconnections = 0;
        std::size_t authenticationAttempts = 0;
        std::size_t welcomeCount = 0;
        std::size_t snapshotCount = 0;
        std::size_t syncCompleteCount = 0;
        std::size_t protocolErrorCount = 0;
        std::size_t parseErrors = 0;
        std::size_t messageErrors = 0;
        std::size_t decodeErrors = 0;
        std::size_t availableMethods = 0;
        std::size_t permittedMethods = 0;
        std::size_t initialSynchronizationBytes = 0;
        std::size_t unexpectedStates = 0;
        bool textFramesOnly = true;
        bool preUpgradeSessionFree = false;
        bool timedOut = false;
    };

    ClientSubProtocol::ClientSubProtocol(web::websocket::SubProtocolContext* context, IntegrationState& state)
        : web::websocket::client::SubProtocol(context, std::string(app::FrontendWebSocketSubProtocolName), 0, 3)
        , state(state) {
    }

    void ClientSubProtocol::onConnected() {
        ++state.websocketConnections;
        frontend::Hello hello;
        hello.capabilities = std::vector{frontend::FrontendCapability::MethodDiscovery, frontend::FrontendCapability::SecurityScopes};
        hello.authentication = frontend::AuthenticationCredential{frontend::BearerCredential{std::string(Bearer)}};
        const auto encoded = frontend::Codec::serializeClient(frontend::ClientMessage{std::move(hello)});
        if (!encoded) {
            state.fail("WSS Hello serialization failed: " + encoded.error().message);
            return;
        }
        sendMessage(encoded.value());
    }

    void ClientSubProtocol::onMessageStart(int opCode) {
        receivedWire.clear();
        state.textFramesOnly = state.textFramesOnly && opCode == web::websocket::SubProtocolContext::OpCode::TEXT;
    }

    void ClientSubProtocol::onMessageData(const char* chunk, std::size_t chunkLength) {
        receivedWire.append(chunk, chunkLength);
    }

    void ClientSubProtocol::onMessageEnd() {
        state.allReceivedWire.append(receivedWire);
        if (receivedWire.find('\n') != std::string::npos || receivedWire.find('\r') != std::string::npos) {
            state.fail("WSS frontend output unexpectedly used JSONL framing");
            return;
        }
        const auto decoded = frontend::Codec::decodeServer(std::string_view(receivedWire));
        if (!decoded) {
            ++state.decodeErrors;
            state.fail("WSS server message decode failed: " + decoded.error().message);
            return;
        }
        if (const auto* welcome = std::get_if<frontend::Welcome>(&decoded.value())) {
            ++state.welcomeCount;
            state.availableMethods = welcome->availableMethods ? welcome->availableMethods->size() : 0;
            state.permittedMethods = welcome->permittedMethods ? welcome->permittedMethods->size() : 0;
        } else if (std::holds_alternative<frontend::Snapshot>(decoded.value())) {
            ++state.snapshotCount;
        } else if (std::holds_alternative<frontend::SyncComplete>(decoded.value())) {
            ++state.syncCompleteCount;
            state.initialSynchronizationBytes = state.allReceivedWire.size();
            sendClose();
        } else if (std::holds_alternative<frontend::ProtocolErrorMessage>(decoded.value())) {
            ++state.protocolErrorCount;
            state.fail("authenticated WSS Hello received a protocol error");
        }
    }

    void ClientSubProtocol::onMessageError([[maybe_unused]] std::uint16_t error) {
        ++state.messageErrors;
        state.fail("WSS client reported a message error");
    }

    void ClientSubProtocol::onDisconnected() {
        ++state.websocketDisconnections;
        core::SNodeC::stop();
    }

    bool ClientSubProtocol::onSignal([[maybe_unused]] int signum) {
        sendClose();
        return false;
    }

    class ClientFactory final : public web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol> {
    public:
        explicit ClientFactory(IntegrationState& state)
            : web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol>(std::string(app::FrontendWebSocketSubProtocolName))
            , state(state) {
        }

    private:
        web::websocket::client::SubProtocol* create(web::websocket::SubProtocolContext* context) override {
            return new ClientSubProtocol(context, state);
        }

        IntegrationState& state;
    };

    IntegrationState* linkedState = nullptr;

    web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol>* createClientFactory() {
        return new ClientFactory(*linkedState);
    }

    int runIntegration(int argc, char* argv[], tests::support::TestResult& result) {
        IntegrationState state(result);
        linkedState = &state;

        core::SNodeC::init(argc, argv);
        int eventLoopResult = 1;
        {
            const auto transport = std::make_shared<tests::codex::FakeTransportState>();
            FakeBackendCore backend({}, transport);
            frontend::FrontendServiceOptions serviceOptions;
            serviceOptions.authenticator = [&state](const frontend::FrontendPeerContext& peer,
                                                    const frontend::AuthenticationCredential& credential) {
                ++state.authenticationAttempts;
                state.authenticatedPeer = peer;
                const auto* bearer = std::get_if<frontend::BearerCredential>(&credential);
                if (bearer == nullptr || bearer->token != Bearer) {
                    return frontend::AuthenticationResult{
                        frontend::AuthenticationFailure{bearer == nullptr ? frontend::AuthenticationFailureCode::AuthenticationRequired
                                                                          : frontend::AuthenticationFailureCode::AuthenticationFailed}};
                }
                return frontend::AuthenticationResult{frontend::AuthenticationSuccess{frontend::FrontendPrincipal{
                    "wss-live-principal",
                    std::vector<frontend::FrontendScope>(frontend::DefaultRemoteScopes.begin(), frontend::DefaultRemoteScopes.end()),
                    "default_remote",
                    false}}};
            };
            frontend::FrontendService service(backend, std::move(serviceOptions));

            app::FrontendWebApplication webApplication(service,
                                                       app::FrontendWebApplicationOptions{
                                                           .endpoint = std::string(Endpoint),
                                                           .staticRoot = std::nullopt,
                                                           .allowedOrigins = {},
                                                           .transport = frontend::FrontendTransportKind::WebSocketTls,
                                                           .encrypted = true,
                                                       });
            express::tls::in::WebApp webApp("a1-7b-wss-integration-server");
            webApplication.configure(webApp);
            result.expectTrue(webApplication.serviceIdentity() == &service,
                              "the production WSS application borrows the application-owned FrontendService");
            webApp.setOnConnect([&state, &service](const auto*) {
                state.preUpgradeSessionFree = service.connectionCount() == 0 && service.unauthenticatedConnectionCount() == 0 &&
                                              service.authenticatedConnectionCount() == 0;
            });

            webApp.getConfig()->Tls::setCert(AISUITE_CODEX_TEST_TLS_CERT);
            webApp.getConfig()->Tls::setCertKey(AISUITE_CODEX_TEST_TLS_KEY);
            webApp.getConfig()->Instance::forceUnrequired();
            webApp.getConfig()->Connection::setMaximumWriteQueueBytes(app::DEFAULT_TRANSPORT_FRAMING_HEADROOM_BYTES +
                                                                      SaturatedApplicationWriterBytes);
            webApp.getConfig()
                ->net::config::ConfigInstance::getSubCommand<web::http::ConfigWebSocket>()
                ->setMaximumFrameBytes(MaximumInboundMessageBytes)
                ->setMaximumMessageBytes(MaximumInboundMessageBytes)
                ->setMaximumFragments(4096);

            web::websocket::client::SocketContextUpgradeFactory::link();
            web::websocket::client::SubProtocolFactorySelector::link(std::string(app::FrontendWebSocketSubProtocolName),
                                                                     createClientFactory);

            webApp.listen(net::in::SocketAddress("127.0.0.1", 0),
                          [&state](const net::in::SocketAddress& address, core::socket::State status) {
                              if (status != core::socket::State::OK || address.getPort() == 0) {
                                  state.fail("IPv4 WSS listener failed to bind: " + status.what());
                                  return;
                              }
                              ++state.serverBinds;

                              const std::string origin = "https://127.0.0.1:" + std::to_string(address.getPort());
                              state.sameOrigin = origin;
                              state.client = std::make_shared<web::http::tls::in::Client>(
                                  "a1-7b-wss-integration-client",
                                  [&state, origin](const auto& request) {
                                      request->set("Host", origin.substr(std::string_view("https://").size()));
                                      request->set("Origin", origin);
                                      request->set("Sec-WebSocket-Protocol", std::string(app::FrontendWebSocketSubProtocolName));
                                      request->upgrade(
                                          std::string(Endpoint),
                                          "websocket",
                                          [&state](bool success) {
                                              if (success) {
                                                  ++state.upgradesStarted;
                                              } else {
                                                  state.fail("WSS upgrade was not initiated");
                                              }
                                          },
                                          [&state](const auto&, const auto& response, bool success) {
                                              if (success && response->get("upgrade") == "websocket") {
                                                  ++state.upgradesCompleted;
                                              } else {
                                                  state.fail("WSS upgrade response failed");
                                              }
                                          },
                                          [&state](const auto&, const std::string&) {
                                              ++state.parseErrors;
                                              state.fail("WSS HTTP response parse failed");
                                          });
                                  },
                                  [](const auto&) {
                                  });
                              state.client->getConfig()->Tls::setCaCert(AISUITE_CODEX_TEST_TLS_CERT);
                              state.client->getConfig()->Tls::setCaCertAcceptUnknown(false);
                              state.client->getConfig()->Tls::setSni("localhost");
                              state.client->getConfig()->Instance::forceUnrequired();
                              state.client->connect(address, [&state](const net::in::SocketAddress&, core::socket::State connectStatus) {
                                  if (connectStatus == core::socket::State::OK) {
                                      ++state.clientConnections;
                                  } else {
                                      state.fail("WSS client TLS connection failed: " + connectStatus.what());
                                  }
                              });
                          });

            [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
                [&state] {
                    state.timedOut = true;
                    state.fail("live WSS integration exceeded its deterministic timeout");
                },
                utils::Timeval({5, 0}));
            eventLoopResult = core::SNodeC::start();
            state.client.reset();
            service.close("WSS integration complete");
        }
        core::SNodeC::free();
        linkedState = nullptr;

        result.expectEqual(0, eventLoopResult, "event loop exits successfully after the live WSS handshake");
        result.expectTrue(!state.timedOut, "the live WSS handshake finishes before the deterministic watchdog");
        result.expectEqual(std::size_t{1}, state.serverBinds, "the production IPv4 WSS listener reports one ephemeral bind");
        result.expectEqual(std::size_t{1}, state.clientConnections, "the certificate-verifying SNode.C TLS client connects once");
        result.expectTrue(state.preUpgradeSessionFree,
                          "the static subprotocol opens no FrontendConnection before a successful TLS WebSocket upgrade");
        result.expectTrue(state.upgradesStarted == 1 && state.upgradesCompleted == 1 && state.websocketConnections == 1 &&
                              state.websocketDisconnections == 1,
                          "one HTTPS request upgrades to one complete WSS connection lifecycle");
        result.expectTrue(state.authenticationAttempts == 1 && state.welcomeCount == 1 && state.snapshotCount == 1 &&
                              state.syncCompleteCount == 1 && state.protocolErrorCount == 0,
                          "one bearer Hello completes Welcome, snapshot, and sync over WSS without a protocol error");
        result.expectTrue(state.availableMethods == 90 && state.permittedMethods == 53,
                          "the authenticated WSS Welcome reports 90 available methods and default_remote 53/90");
        // Initial synchronization is one synchronous ServerCore flush, so
        // write events cannot drain this aggregate before the adapter reaches
        // its application budget and schedules the retained-head retry.
        result.expectTrue(state.initialSynchronizationBytes > SaturatedApplicationWriterBytes,
                          "the lossless WSS synchronization exceeds its application writer allowance and completes after retry");
        result.expectTrue(state.authenticatedPeer.has_value() &&
                              state.authenticatedPeer->transport == frontend::FrontendTransportKind::WebSocketTls &&
                              state.authenticatedPeer->encrypted && state.authenticatedPeer->loopback &&
                              !state.authenticatedPeer->localPeer && state.authenticatedPeer->remoteAddress == "127.0.0.1",
                          "the production WSS adapter propagates an encrypted loopback peer without claiming local trust");
        result.expectTrue(state.sameOrigin.has_value() && state.authenticatedPeer.has_value() &&
                              state.authenticatedPeer->origin == state.sameOrigin,
                          "the production Origin policy accepts the exact same-origin HTTPS form");
        result.expectTrue(state.textFramesOnly && state.parseErrors == 0 && state.messageErrors == 0 && state.decodeErrors == 0,
                          "all WSS frontend output uses complete text messages without HTTP or protocol parse errors");
        result.expectTrue(state.allReceivedWire.find(Bearer) == std::string::npos,
                          "the synthetic bearer is absent from all server WSS output");
        result.expectEqual(std::size_t{0}, state.unexpectedStates, "the live WSS scenario reports no unexpected state");
        return result.processResult();
    }

} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    if (tests::support::shouldSkipRootWithoutSNodeCGroup()) {
        tests::support::printRootWithoutSNodeCGroupSkipMessage("CodexFrontendWebSocketTlsIntegrationTest");
        return tests::support::cTestSkipReturnCode;
    }
    return runIntegration(argc, argv, result);
}
