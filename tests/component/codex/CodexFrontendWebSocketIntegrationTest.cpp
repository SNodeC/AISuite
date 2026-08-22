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
#include "apps/codex-backend/FrontendStreamSocketContextFactory.h"
#include "apps/codex-backend/FrontendWebApplication.h"
#include "core/SNodeC.h"
#include "core/socket/State.h"
#include "core/timer/Timer.h"
#include "express/legacy/in/WebApp.h"
#include "log/SemanticLogger.h"
#include "net/in/SocketAddress.h"
#include "net/in/stream/legacy/SocketServer.h"
#include "support/SemanticLogCapture.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"
#include "web/http/ConfigHttpParser.h"
#include "web/http/ConfigWebSocket.h"
#include "web/http/legacy/in/Client.h"
#include "web/http/server/ConfigHttpServer.h"
#include "web/websocket/SubProtocolContext.h"
#include "web/websocket/SubProtocolFactory.h"
#include "web/websocket/client/SocketContextUpgradeFactory.h"
#include "web/websocket/client/SubProtocol.h"
#include "web/websocket/client/SubProtocolFactorySelector.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
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

    constexpr std::string_view ValidBearer = "a1-7b-websocket-live-token";
    constexpr std::string_view InvalidBearer = "a1-7b-websocket-invalid-token";
    constexpr std::string_view Endpoint = "/frontend";
    constexpr std::size_t MaximumInboundMessageBytes = 512;
    constexpr std::size_t SaturatedApplicationWriterBytes = 6U * 1024U;

    enum class CaseKind : std::size_t { WrongSubprotocol, MissingBearer, BadBearer, Binary, OversizedText, GoodText, Count };

    constexpr std::size_t caseIndex(CaseKind kind) noexcept {
        return static_cast<std::size_t>(kind);
    }

    std::string_view caseName(CaseKind kind) noexcept {
        switch (kind) {
            case CaseKind::WrongSubprotocol:
                return "wrong subprotocol";
            case CaseKind::MissingBearer:
                return "missing bearer";
            case CaseKind::BadBearer:
                return "bad bearer";
            case CaseKind::GoodText:
                return "good text";
            case CaseKind::Binary:
                return "binary";
            case CaseKind::OversizedText:
                return "oversized text";
            case CaseKind::Count:
                break;
        }
        return "unknown";
    }

    bool hasCapability(const frontend::Welcome& welcome, frontend::FrontendCapability capability) {
        return welcome.capabilities &&
               std::find(welcome.capabilities->implemented.begin(), welcome.capabilities->implemented.end(), capability) !=
                   welcome.capabilities->implemented.end();
    }

    struct CaseObservation {
        std::size_t httpConnected = 0;
        std::size_t connectSucceeded = 0;
        std::size_t upgradeInitiated = 0;
        std::size_t upgradeCompleted = 0;
        std::size_t websocketConnected = 0;
        std::size_t websocketDisconnected = 0;
        std::size_t messageStarts = 0;
        std::size_t messageEnds = 0;
        std::size_t parseErrors = 0;
        std::size_t messageErrors = 0;
        std::size_t decodeErrors = 0;
        std::size_t welcomeCount = 0;
        std::size_t snapshotCount = 0;
        std::size_t syncCompleteCount = 0;
        std::size_t responseCount = 0;
        std::size_t protocolErrorCount = 0;
        std::size_t availableMethods = 0;
        std::size_t permittedMethods = 0;
        std::size_t initialSynchronizationBytes = 0;
        std::optional<frontend::ErrorCode> protocolError;
        bool textOpcodeOnly = true;
        bool advertisedMultiTransport = false;
        bool snapshotResponseOk = false;
        bool completed = false;
        std::string receivedWire;
        std::string allReceivedWire;
    };

    class ClientSubProtocol;

    struct IntegrationState {
        explicit IntegrationState(tests::support::TestResult& result)
            : result(result) {
        }

        void fail(std::string message) {
            ++unexpectedStates;
            result.expectTrue(false, std::move(message));
            core::SNodeC::stop();
        }

        void clientConnected(CaseKind kind, ClientSubProtocol& protocol);
        void serverMessage(CaseKind kind, frontend::ServerMessage message, ClientSubProtocol& protocol);
        void clientDisconnected(CaseKind kind);
        void dispatchNext();

        tests::support::TestResult& result;
        std::function<void(CaseKind)> connector;
        std::array<CaseObservation, caseIndex(CaseKind::Count)> cases{};
        std::vector<frontend::FrontendPeerContext> authenticatedPeers;
        std::vector<std::shared_ptr<web::http::legacy::in::Client>> clients;
        std::size_t activeCase = 0;
        std::size_t listenerSuccesses = 0;
        std::size_t listenerFailures = 0;
        std::size_t authenticationAttempts = 0;
        std::size_t unexpectedStates = 0;
        std::optional<std::string> goodSameOrigin;
        bool capabilitiesObservedBeforeClients = false;
        bool timedOut = false;
    };

    class ClientSubProtocol final : public web::websocket::client::SubProtocol {
    public:
        ClientSubProtocol(web::websocket::SubProtocolContext* context, IntegrationState& state, CaseKind kind)
            : web::websocket::client::SubProtocol(context, std::string(app::FrontendWebSocketSubProtocolName), 0, 3)
            , state(state)
            , kind(kind) {
        }

        void sendFrontend(frontend::ClientMessage message) {
            const auto encoded = frontend::Codec::serializeClient(message);
            if (!encoded) {
                state.fail(std::string(caseName(kind)) + " client serialization failed: " + encoded.error().message);
                return;
            }
            // The std::string overload emits one complete TEXT message. No
            // JSONL delimiter exists on the WebSocket transport.
            sendMessage(encoded.value());
        }

        void closeNormally() {
            sendClose();
        }

    private:
        void onConnected() override {
            ++state.cases[caseIndex(kind)].websocketConnected;
            state.clientConnected(kind, *this);
        }

        void onMessageStart(int opCode) override {
            CaseObservation& observation = state.cases[caseIndex(kind)];
            ++observation.messageStarts;
            observation.receivedWire.clear();
            observation.textOpcodeOnly = observation.textOpcodeOnly && opCode == web::websocket::SubProtocolContext::OpCode::TEXT;
        }

        void onMessageData(const char* chunk, std::size_t chunkLength) override {
            state.cases[caseIndex(kind)].receivedWire.append(chunk, chunkLength);
        }

        void onMessageEnd() override {
            CaseObservation& observation = state.cases[caseIndex(kind)];
            ++observation.messageEnds;
            observation.allReceivedWire.append(observation.receivedWire);
            if (observation.receivedWire.find('\n') != std::string::npos || observation.receivedWire.find('\r') != std::string::npos) {
                state.fail(std::string(caseName(kind)) + " server WebSocket message unexpectedly used JSONL framing");
                return;
            }
            const auto decoded = frontend::Codec::decodeServer(std::string_view(observation.receivedWire));
            if (!decoded) {
                ++observation.decodeErrors;
                state.fail(std::string(caseName(kind)) + " server message decode failed: " + decoded.error().message);
                return;
            }
            state.serverMessage(kind, decoded.value(), *this);
        }

        void onMessageError([[maybe_unused]] std::uint16_t error) override {
            ++state.cases[caseIndex(kind)].messageErrors;
            state.fail(std::string(caseName(kind)) + " WebSocket client reported a message error");
        }

        void onDisconnected() override {
            ++state.cases[caseIndex(kind)].websocketDisconnected;
            state.clientDisconnected(kind);
        }

        bool onSignal([[maybe_unused]] int signum) override {
            sendClose();
            return false;
        }

        IntegrationState& state;
        CaseKind kind;
    };

    void IntegrationState::clientConnected(CaseKind kind, ClientSubProtocol& protocol) {
        frontend::Hello hello;
        hello.capabilities = std::vector{frontend::FrontendCapability::MethodDiscovery, frontend::FrontendCapability::SecurityScopes};
        switch (kind) {
            case CaseKind::WrongSubprotocol:
                fail("wrong-subprotocol case unexpectedly established a WebSocket");
                break;
            case CaseKind::MissingBearer:
                protocol.sendFrontend(frontend::ClientMessage{std::move(hello)});
                break;
            case CaseKind::BadBearer:
                hello.authentication = frontend::AuthenticationCredential{frontend::BearerCredential{std::string(InvalidBearer)}};
                protocol.sendFrontend(frontend::ClientMessage{std::move(hello)});
                break;
            case CaseKind::GoodText:
                hello.authentication = frontend::AuthenticationCredential{frontend::BearerCredential{std::string(ValidBearer)}};
                protocol.sendFrontend(frontend::ClientMessage{std::move(hello)});
                break;
            case CaseKind::Binary: {
                hello.authentication = frontend::AuthenticationCredential{frontend::BearerCredential{std::string(ValidBearer)}};
                const auto encoded = frontend::Codec::serializeClient(frontend::ClientMessage{std::move(hello)});
                if (!encoded) {
                    fail("binary-case Hello serialization failed: " + encoded.error().message);
                    return;
                }
                // The pointer/length overload deliberately emits BINARY.
                protocol.sendMessage(encoded.value().data(), encoded.value().size());
                break;
            }
            case CaseKind::OversizedText:
                protocol.sendMessage(std::string(MaximumInboundMessageBytes + 1, 'x'));
                break;
            case CaseKind::Count:
                fail("invalid WebSocket integration case");
                break;
        }
    }

    void IntegrationState::serverMessage(CaseKind kind, frontend::ServerMessage message, ClientSubProtocol& protocol) {
        CaseObservation& observation = cases[caseIndex(kind)];
        if (const auto* welcome = std::get_if<frontend::Welcome>(&message)) {
            ++observation.welcomeCount;
            observation.availableMethods = welcome->availableMethods ? welcome->availableMethods->size() : 0;
            observation.permittedMethods = welcome->permittedMethods ? welcome->permittedMethods->size() : 0;
            observation.advertisedMultiTransport = hasCapability(*welcome, frontend::FrontendCapability::MultiTransport);
        } else if (std::holds_alternative<frontend::Snapshot>(message)) {
            ++observation.snapshotCount;
        } else if (std::holds_alternative<frontend::SyncComplete>(message)) {
            ++observation.syncCompleteCount;
            if (kind == CaseKind::GoodText && observation.syncCompleteCount == 1) {
                observation.initialSynchronizationBytes = observation.allReceivedWire.size();
                protocol.sendFrontend(frontend::ClientMessage{
                    frontend::Command{"websocket-snapshot", frontend::SnapshotGet{}, frontend::Json::object(), frontend::Json::object()}});
            }
        } else if (const auto* response = std::get_if<frontend::Response>(&message)) {
            ++observation.responseCount;
            if (kind == CaseKind::GoodText && response->requestId == "websocket-snapshot") {
                observation.snapshotResponseOk = response->ok;
                protocol.closeNormally();
            }
        } else if (const auto* error = std::get_if<frontend::ProtocolErrorMessage>(&message)) {
            ++observation.protocolErrorCount;
            observation.protocolError = error->code;
        }
    }

    void IntegrationState::clientDisconnected(CaseKind kind) {
        CaseObservation& observation = cases[caseIndex(kind)];
        if (observation.completed) {
            return;
        }
        observation.completed = true;
        if (caseIndex(kind) != activeCase) {
            fail("WebSocket cases completed out of deterministic order");
            return;
        }
        ++activeCase;
        dispatchNext();
    }

    void IntegrationState::dispatchNext() {
        if (activeCase == caseIndex(CaseKind::Count)) {
            core::SNodeC::stop();
            return;
        }
        if (!connector) {
            fail("WebSocket integration connector is unavailable");
            return;
        }
        connector(static_cast<CaseKind>(activeCase));
    }

    class ClientFactory final : public web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol> {
    public:
        explicit ClientFactory(IntegrationState& state)
            : web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol>(std::string(app::FrontendWebSocketSubProtocolName))
            , state(state) {
        }

    private:
        web::websocket::client::SubProtocol* create(web::websocket::SubProtocolContext* context) override {
            return new ClientSubProtocol(context, state, static_cast<CaseKind>(state.activeCase));
        }

        IntegrationState& state;
    };

    IntegrationState* linkedState = nullptr;

    web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol>* createClientFactory() {
        return new ClientFactory(*linkedState);
    }

    frontend::AuthenticationResult
    authenticate(IntegrationState& state, const frontend::FrontendPeerContext& peer, const frontend::AuthenticationCredential& credential) {
        ++state.authenticationAttempts;
        state.authenticatedPeers.push_back(peer);
        const auto* bearer = std::get_if<frontend::BearerCredential>(&credential);
        if (bearer == nullptr || bearer->token != ValidBearer) {
            return frontend::AuthenticationFailure{bearer == nullptr ? frontend::AuthenticationFailureCode::AuthenticationRequired
                                                                     : frontend::AuthenticationFailureCode::AuthenticationFailed};
        }
        return frontend::AuthenticationSuccess{frontend::FrontendPrincipal{
            "websocket-live-principal",
            std::vector<frontend::FrontendScope>(frontend::DefaultRemoteScopes.begin(), frontend::DefaultRemoteScopes.end()),
            "default_remote",
            false}};
    }

    int runIntegration(int argc, char* argv[], tests::support::TestResult& result) {
        static_cast<void>(argc);
        static_cast<void>(argv);
        IntegrationState state(result);
        linkedState = &state;
        tests::support::SemanticLogCapture logCapture("aisuite-a1-7b-frontend-websocket-live");
        logCapture.initCore("CodexFrontendWebSocketIntegrationTest");
        int eventLoopResult = 1;
        {
            const auto transport = std::make_shared<tests::codex::FakeTransportState>();
            FakeBackendCore backend({}, transport);
            frontend::FrontendServiceOptions serviceOptions;
            serviceOptions.authenticator = [&state](const frontend::FrontendPeerContext& peer,
                                                    const frontend::AuthenticationCredential& credential) {
                return authenticate(state, peer, credential);
            };
            frontend::FrontendService service(backend, std::move(serviceOptions));

            app::FrontendWebApplication webApplication(service,
                                                       app::FrontendWebApplicationOptions{
                                                           .endpoint = std::string(Endpoint),
                                                           .staticRoot = std::nullopt,
                                                           .allowedOrigins = {},
                                                           .transport = frontend::FrontendTransportKind::WebSocket,
                                                           .encrypted = false,
                                                       });
            express::legacy::in::WebApp webApp("a1-7b-websocket-integration-server");
            webApplication.configure(webApp);
            result.expectTrue(webApplication.serviceIdentity() == &service,
                              "the production WebSocket application borrows the one application-owned FrontendService");

            auto nativeServer = net::in::stream::legacy::Server<app::FrontendStreamSocketContextFactory>(
                "a1-7b-websocket-shared-native-server",
                [](net::in::stream::legacy::config::ConfigSocketServer* config) {
                    config->Instance::setDisabled(false);
                    config->Local::setHost("127.0.0.1")->setPort(0);
                    config->Connection::setMaximumWriteQueueBytes(app::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
                },
                service,
                app::FrontendStreamSocketContextFactoryOptions{
                    .transport = frontend::FrontendTransportKind::Ipv4, .socket = {}, .resolvePeer = {}});
            webApp.getConfig()->Instance::forceUnrequired();
            webApp.getConfig()->Connection::setMaximumWriteQueueBytes(app::DEFAULT_TRANSPORT_FRAMING_HEADROOM_BYTES +
                                                                      SaturatedApplicationWriterBytes);
            nativeServer.getConfig()->Instance::forceUnrequired();
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
                ->setMaximumFrameBytes(MaximumInboundMessageBytes)
                ->setMaximumMessageBytes(MaximumInboundMessageBytes)
                ->setMaximumFragments(64);

            std::optional<std::uint16_t> websocketPort;
            bool clientStarted = false;
            const auto startClientWhenReady = [&state, &service, &clientStarted] {
                if (clientStarted || state.listenerSuccesses != 2) {
                    return;
                }
                clientStarted = true;
                const auto capabilities = service.implementedCapabilities();
                state.capabilitiesObservedBeforeClients =
                    service.connectionCount() == 0 && capabilities.size() == 16 &&
                    std::find(capabilities.begin(), capabilities.end(), frontend::FrontendCapability::MultiTransport) != capabilities.end();
                state.dispatchNext();
            };

            state.connector = [&state, &websocketPort](CaseKind kind) {
                if (!websocketPort) {
                    state.fail("WebSocket client dispatched without a bound endpoint");
                    return;
                }
                const std::string sameOrigin = "http://127.0.0.1:" + std::to_string(*websocketPort);
                if (kind == CaseKind::GoodText) {
                    state.goodSameOrigin = sameOrigin;
                }
                auto client = std::make_shared<web::http::legacy::in::Client>(
                    "a1-7b-websocket-integration-client-" + std::to_string(caseIndex(kind)),
                    [&state, kind, sameOrigin](const auto& request) {
                        ++state.cases[caseIndex(kind)].httpConnected;
                        request->set("Sec-WebSocket-Protocol",
                                     kind == CaseKind::WrongSubprotocol ? "credential-sentinel-subprotocol"
                                                                        : std::string(app::FrontendWebSocketSubProtocolName));
                        if (kind == CaseKind::GoodText) {
                            request->set("Origin", sameOrigin);
                        }
                        request->upgrade(
                            std::string(Endpoint),
                            "websocket",
                            [&state, kind](bool success) {
                                if (success) {
                                    ++state.cases[caseIndex(kind)].upgradeInitiated;
                                } else {
                                    state.fail(std::string(caseName(kind)) + " WebSocket upgrade was not initiated");
                                }
                            },
                            [&state, kind](const auto&, const auto& response, bool success) {
                                if (kind == CaseKind::WrongSubprotocol) {
                                    if (!success && response->statusCode == "400") {
                                        state.clientDisconnected(kind);
                                    } else {
                                        state.fail("wrong subprotocol was not rejected with HTTP 400");
                                    }
                                    return;
                                }
                                if (success && response->get("upgrade") == "websocket") {
                                    ++state.cases[caseIndex(kind)].upgradeCompleted;
                                } else {
                                    state.fail(std::string(caseName(kind)) + " WebSocket upgrade response failed");
                                }
                            },
                            [&state, kind](const auto&, const std::string&) {
                                ++state.cases[caseIndex(kind)].parseErrors;
                                state.fail(std::string(caseName(kind)) + " WebSocket HTTP response parse failed");
                            });
                    },
                    [](const auto&) {
                    });
                client->getConfig()->Instance::forceUnrequired();
                client->connect(net::in::SocketAddress("127.0.0.1", *websocketPort),
                                [&state, kind](const net::in::SocketAddress&, core::socket::State connectState) {
                                    if (connectState == core::socket::State::OK) {
                                        ++state.cases[caseIndex(kind)].connectSucceeded;
                                    } else {
                                        state.fail(std::string(caseName(kind)) +
                                                   " WebSocket client connection failed: " + connectState.what());
                                    }
                                });
                state.clients.push_back(std::move(client));
            };

            web::websocket::client::SocketContextUpgradeFactory::link();
            web::websocket::client::SubProtocolFactorySelector::link(std::string(app::FrontendWebSocketSubProtocolName),
                                                                     createClientFactory);

            webApp.listen(net::in::SocketAddress("127.0.0.1", 0),
                          [&state, &service, &websocketPort, &startClientWhenReady](const net::in::SocketAddress& address,
                                                                                    core::socket::State status) {
                              if (status != core::socket::State::OK || address.getPort() == 0) {
                                  ++state.listenerFailures;
                                  state.fail("IPv4 WebSocket listener failed to bind: " + status.what());
                                  return;
                              }
                              service.declareTransportFamily(frontend::FrontendTransportKind::WebSocket);
                              websocketPort = address.getPort();
                              ++state.listenerSuccesses;
                              startClientWhenReady();
                          });
            nativeServer.listen(
                [&state, &service, &startClientWhenReady](const net::in::SocketAddress& address, core::socket::State status) {
                    if (status != core::socket::State::OK || address.getPort() == 0) {
                        ++state.listenerFailures;
                        state.fail("shared IPv4 JSONL listener failed to bind: " + status.what());
                        return;
                    }
                    service.declareTransportFamily(frontend::FrontendTransportKind::Ipv4);
                    ++state.listenerSuccesses;
                    startClientWhenReady();
                });

            [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
                [&state] {
                    state.timedOut = true;
                    state.fail("live WebSocket integration exceeded its deterministic timeout");
                },
                utils::Timeval({5, 0}));
            eventLoopResult = core::SNodeC::start();
            state.clients.clear();
            service.close("WebSocket integration complete");
        }
        core::SNodeC::free();
        linkedState = nullptr;
        const std::vector<nlohmann::json> logRecords = logCapture.finish();

        result.expectEqual(0, eventLoopResult, "event loop exits successfully after the live IPv4 WebSocket cases");
        result.expectTrue(!state.timedOut, "live WebSocket cases finish before the deterministic watchdog");
        result.expectEqual(
            std::size_t{2}, state.listenerSuccesses, "the WebSocket and shared native listener each report one successful ephemeral bind");
        result.expectEqual(std::size_t{0}, state.listenerFailures, "both live listeners bind without failure");
        result.expectTrue(state.capabilitiesObservedBeforeClients,
                          "the shared service advertises fourteen mechanisms, cpp_client_sdk, and topology-derived multi_transport");
        result.expectEqual(caseIndex(CaseKind::Count), state.activeCase, "all six live WebSocket cases complete in order");
        result.expectEqual(std::size_t{3},
                           state.authenticationAttempts,
                           "the missing, bad, and good text Hello messages reach authentication exactly once");
        result.expectTrue(state.authenticatedPeers.size() == 3 &&
                              std::ranges::all_of(state.authenticatedPeers,
                                                  [](const frontend::FrontendPeerContext& peer) {
                                                      return peer.transport == frontend::FrontendTransportKind::WebSocket &&
                                                             !peer.encrypted && peer.loopback && !peer.localPeer &&
                                                             peer.remoteAddress == "127.0.0.1";
                                                  }),
                          "the production WebSocket context propagates bounded loopback peer facts without claiming local trust");
        result.expectTrue(state.goodSameOrigin.has_value() && state.authenticatedPeers.size() == 3 &&
                              !state.authenticatedPeers[0].origin.has_value() && !state.authenticatedPeers[1].origin.has_value() &&
                              state.authenticatedPeers[2].origin == state.goodSameOrigin,
                          "the production Origin policy accepts the exact browser same-origin form while a native client omits Origin");

        const CaseObservation& wrongSubprotocol = state.cases[caseIndex(CaseKind::WrongSubprotocol)];
        result.expectTrue(wrongSubprotocol.completed && wrongSubprotocol.websocketConnected == 0 && wrongSubprotocol.welcomeCount == 0,
                          "an unapproved WebSocket subprotocol is rejected before frontend authentication");

        const CaseObservation& missing = state.cases[caseIndex(CaseKind::MissingBearer)];
        result.expectTrue(missing.completed && missing.protocolErrorCount == 1 &&
                              missing.protocolError == frontend::ErrorCode::AuthenticationRequired && missing.welcomeCount == 0,
                          "a missing remote bearer receives one authentication_required error and no Welcome");

        const CaseObservation& bad = state.cases[caseIndex(CaseKind::BadBearer)];
        result.expectTrue(bad.completed && bad.protocolErrorCount == 1 && bad.protocolError == frontend::ErrorCode::AuthenticationFailed &&
                              bad.welcomeCount == 0,
                          "a bad bearer receives one generic authentication_failed error and no Welcome (errors=" +
                              std::to_string(bad.protocolErrorCount) + ", messages=" + std::to_string(bad.messageEnds) +
                              ", welcomes=" + std::to_string(bad.welcomeCount) + ")");

        const CaseObservation& good = state.cases[caseIndex(CaseKind::GoodText)];
        result.expectTrue(good.completed && good.welcomeCount == 1 && good.snapshotCount == 2 && good.syncCompleteCount == 2 &&
                              good.responseCount == 1 && good.snapshotResponseOk,
                          "a subsequent good bearer completes Hello/Welcome/snapshot/sync and one exact text command response (welcome=" +
                              std::to_string(good.welcomeCount) + ", snapshot=" + std::to_string(good.snapshotCount) +
                              ", sync=" + std::to_string(good.syncCompleteCount) + ", responses=" + std::to_string(good.responseCount) +
                              ", messages=" + std::to_string(good.messageEnds) + ")");
        result.expectTrue(good.availableMethods == 90 && good.permittedMethods == 53 && good.advertisedMultiTransport,
                          "the live Welcome reports 90 available methods, default_remote 53/90, and multi_transport for two families");
        // Welcome, Snapshot, and SyncComplete are attempted by one
        // synchronous ServerCore flush. Socket write events cannot interleave
        // with that callback, so exceeding the configured application budget
        // forces the adapter through Backpressured and its deferred retry.
        result.expectTrue(good.initialSynchronizationBytes > SaturatedApplicationWriterBytes,
                          "the lossless WebSocket synchronization exceeds its application writer allowance and completes after retry");

        const CaseObservation& binary = state.cases[caseIndex(CaseKind::Binary)];
        result.expectTrue(binary.completed && binary.websocketDisconnected == 1 && binary.messageEnds == 0 && binary.welcomeCount == 0,
                          "a binary frontend message is rejected connection-locally before authentication");

        const CaseObservation& oversized = state.cases[caseIndex(CaseKind::OversizedText)];
        result.expectTrue(oversized.completed && oversized.websocketDisconnected == 1 && oversized.welcomeCount == 0,
                          "an oversized text message is bounded and closes only that WebSocket connection");
        result.expectTrue(oversized.protocolErrorCount <= 1 &&
                              (!oversized.protocolError || oversized.protocolError == frontend::ErrorCode::FrameTooLarge),
                          "the oversized message emits at most one bounded frame_too_large protocol error");

        bool allTextFrames = true;
        bool sentinelAbsent = true;
        for (const CaseObservation& observation : state.cases) {
            allTextFrames = allTextFrames && observation.textOpcodeOnly && observation.parseErrors == 0 && observation.messageErrors == 0 &&
                            observation.decodeErrors == 0;
            sentinelAbsent = sentinelAbsent && observation.allReceivedWire.find(ValidBearer) == std::string::npos &&
                             observation.allReceivedWire.find(InvalidBearer) == std::string::npos;
        }
        for (const nlohmann::json& record : logRecords) {
            const std::string serialized = record.dump();
            sentinelAbsent =
                sentinelAbsent && serialized.find(ValidBearer) == std::string::npos && serialized.find(InvalidBearer) == std::string::npos;
            if (record.value("role", std::string{}) == "server") {
                sentinelAbsent = sentinelAbsent && serialized.find("credential-sentinel-subprotocol") == std::string::npos;
            }
        }
        result.expectTrue(allTextFrames, "all server frontend messages use complete WebSocket TEXT frames without parser failures");
        result.expectTrue(sentinelAbsent, "synthetic credential sentinels appear in neither server WebSocket output nor semantic logs");
        result.expectEqual(std::size_t{0}, state.unexpectedStates, "the live WebSocket scenario reports no unexpected state");
        return result.processResult();
    }

} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    if (tests::support::shouldSkipRootWithoutSNodeCGroup()) {
        tests::support::printRootWithoutSNodeCGroupSkipMessage("CodexFrontendWebSocketIntegrationTest");
        return tests::support::cTestSkipReturnCode;
    }
    return runIntegration(argc, argv, result);
}
