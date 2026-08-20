/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/internal/server/ServerCore.h"
#include "support/TestResult.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = ai::openai::codex::frontend::generated;
    namespace model = ai::openai::codex::frontend::internal::model;
    namespace server = ai::openai::codex::frontend::internal::server;

    class Backend final : public server::BackendPort {
    public:
        [[nodiscard]] bool providerReady() const noexcept override {
            return true;
        }

        [[nodiscard]] model::CanonicalSnapshot snapshot() const override {
            if (throwSnapshot) {
                throw std::runtime_error("injected snapshot failure");
            }
            return state;
        }

        [[nodiscard]] server::BackendSubmitStatus submit(server::BackendInvocation invocation) override {
            ++submissionCount;
            if (core && (invocation.token.method == generated::MethodId::ControllerAcquire ||
                         invocation.token.method == generated::MethodId::ControllerRelease)) {
                const bool acquire = invocation.token.method == generated::MethodId::ControllerAcquire;
                controller = acquire ? std::optional<model::SessionIdentity>{invocation.session} : std::nullopt;
                controllerSessions.push_back(acquire ? invocation.session.value() : std::string{});
                if (onControllerChanged) {
                    std::function<void()> callback = std::move(onControllerChanged);
                    callback();
                }
                static_cast<void>(core->complete(server::BackendCompletion{
                    invocation.token,
                    server::BackendCommandSuccess{generated::makeResult(
                        invocation.token.method,
                        acquire ? frontend::Json{{"controllerSessionId", invocation.session.value()}, {"role", "controller"}}
                                : frontend::Json{{"role", "observer"}})}}));
            }
            return server::BackendSubmitStatus::Accepted;
        }

        void bind(server::ServerCore& boundCore) noexcept override {
            core = &boundCore;
        }

        void unbind(server::ServerCore& boundCore) noexcept override {
            if (core == &boundCore) {
                core = nullptr;
            }
        }

        [[nodiscard]] bool performProviderLifecycleAction(server::ProviderLifecycleAction) override {
            return true;
        }

        void sessionOpened(const model::SessionIdentity& session, const frontend::FrontendPrincipal& principal) override {
            openedSessions.push_back(session.value());
            principalIds.push_back(principal.id);
            if (onSessionOpened) {
                std::function<void()> callback = std::move(onSessionOpened);
                callback();
            }
        }

        void sessionClosed(const model::SessionIdentity& session) noexcept override {
            closedSessions.push_back(session.value());
            if (controller && *controller == session) {
                controller.reset();
                controllerSessions.emplace_back();
                if (onControllerChanged) {
                    std::function<void()> callback = std::move(onControllerChanged);
                    callback();
                }
            }
        }

        void controllerChanged(const std::optional<model::SessionIdentity>& session) noexcept override {
            controllerSessions.push_back(session ? session->value() : std::string{});
            if (onControllerChanged) {
                std::function<void()> callback = std::move(onControllerChanged);
                callback();
            }
        }

        model::CanonicalSnapshot state;
        bool throwSnapshot = false;
        std::vector<std::string> openedSessions;
        std::vector<std::string> closedSessions;
        std::vector<std::string> principalIds;
        std::vector<std::string> controllerSessions;
        std::size_t submissionCount = 0;
        std::function<void()> onSessionOpened;
        std::function<void()> onControllerChanged;
        server::ServerCore* core = nullptr;
        std::optional<model::SessionIdentity> controller;
    };

    struct Timer {
        std::function<void()> callback;
        bool cancelled = false;
    };

    frontend::AuthenticationResult authenticate(const frontend::FrontendPeerContext&, const frontend::AuthenticationCredential&) {
        frontend::FrontendPrincipal principal;
        principal.id = "lifecycle-principal";
        principal.profile = "test";
        principal.scopes = {frontend::FrontendScope::Observe};
        return frontend::AuthenticationSuccess{std::move(principal)};
    }

    model::OccurrenceDraft providerOccurrence(std::string source) {
        model::ProviderState provider;
        provider.lifecycle = model::ProviderLifecycle::Ready;
        return {model::SourceStamp{"backend-event:" + source}, model::ProviderUpdatedOccurrence{std::move(provider)}};
    }

    bool containsCapability(const std::vector<frontend::FrontendCapability>& capabilities, frontend::FrontendCapability capability) {
        return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
    }

    frontend::Json wrongProtocolMessage() {
        return {{"protocol", "not-the-frontend-protocol"},
                {"version", frontend::ProtocolVersion},
                {"kind", frontend::kind::Hello}};
    }

    void testLifecycle(tests::support::TestResult& result) {
        Backend backend;
        std::vector<std::shared_ptr<Timer>> timers;
        server::ServerCoreOptions options;
        options.maxConnections = 2;
        options.maxUnauthenticatedConnections = 1;
        options.authenticator = authenticate;
        options.timerScheduler = [&timers](std::uint64_t, std::function<void()> callback) {
            auto timer = std::make_shared<Timer>(Timer{std::move(callback), false});
            timers.push_back(timer);
            return [timer] {
                timer->cancelled = true;
            };
        };

        server::ServerCore core(backend, std::move(options));
        std::vector<frontend::ServerMessage> firstMessages;
        std::vector<server::ConnectionClose> closes;
        const auto callbacks = [&](std::vector<frontend::ServerMessage>& messages) {
            return server::ConnectionCallbacks{[&messages](server::SerializedServerMessage outbound) {
                                                   messages.push_back(std::move(outbound.message));
                                                   return true;
                                               },
                                               [&closes](const server::ConnectionClose& close) {
                                                   closes.push_back(close);
                                               }};
        };

        result.expectTrue(!core.openConnection({}, callbacks(firstMessages)),
                          "connections are rejected until the transport-neutral service is started");
        core.start();
        const auto first = core.openConnection({}, callbacks(firstMessages));
        result.expectTrue(first.has_value() && core.connectionCount() == 1 && core.unauthenticatedConnectionCount() == 1,
                          "start admits one bounded unauthenticated connection");
        result.expectTrue(!core.openConnection({}, callbacks(firstMessages)),
                          "the independent unauthenticated connection ceiling is enforced");

        frontend::FrontendPeerContext updatedPeer;
        updatedPeer.transport = frontend::FrontendTransportKind::InMemory;
        updatedPeer.remoteAddress = "deterministic-peer";
        result.expectTrue(core.updatePeerContext(*first, updatedPeer), "peer facts can be completed before the one-shot Hello attempt");
        const auto observedPeer = core.peer(*first);
        result.expectTrue(core.connectionOpen(*first) && observedPeer && observedPeer->remoteAddress == updatedPeer.remoteAddress &&
                              !core.principal(*first),
                          "the compatibility seam exposes immutable peer facts and no principal before Hello");
        const server::ReceiveResult hello = core.receive(*first, frontend::ClientMessage{frontend::Hello{}});
        const auto* welcome = !firstMessages.empty() ? std::get_if<frontend::Welcome>(&firstMessages.front()) : nullptr;
        result.expectTrue(hello.accepted() && welcome != nullptr && welcome->sessionId == "1" && !welcome->capabilities &&
                              !welcome->availableMethods && !welcome->permittedMethods && firstMessages.size() == 4 &&
                              std::holds_alternative<frontend::EventBatch>(firstMessages.back()),
                          "Hello creates a canonical session, synchronizes it, then publishes the core-owned session occurrence");
        result.expectTrue(core.helloComplete(*first) && core.authenticatedConnectionCount() == 1 &&
                              core.unauthenticatedConnectionCount() == 0 && backend.openedSessions == std::vector<std::string>{"1"} &&
                              timers.front()->cancelled && core.principal(*first) && core.principal(*first)->id == "lifecycle-principal",
                          "successful authentication opens one backend session and cancels its handshake timer");
        result.expectTrue(!core.updatePeerContext(*first, {}), "authenticated peer facts are immutable");

        std::vector<frontend::ServerMessage> secondMessages;
        const auto second = core.openConnection({}, callbacks(secondMessages));
        result.expectTrue(second.has_value() && timers.size() == 2,
                          "authentication frees the unauthenticated slot for another physical connection");
        timers.back()->callback();
        result.expectTrue(core.connectionCount() == 1 && !closes.empty() &&
                              closes.back().protocolCode == frontend::ErrorCode::AuthenticationRequired,
                          "the injected handshake timer closes only its stale unauthenticated connection");

        core.close("test shutdown");
        result.expectTrue(!core.isOpen() && core.connectionCount() == 0 && backend.closedSessions == std::vector<std::string>{"1"} &&
                              closes.back().clean,
                          "clean service shutdown is idempotent and closes each authenticated backend session once");
        core.close("duplicate shutdown");
        result.expectTrue(backend.closedSessions.size() == 1, "a second service close has no lifecycle side effects");
        core.start();
        result.expectTrue(!core.isOpen() && !core.openConnection({}, callbacks(firstMessages)),
                          "service close is terminal and a later start cannot reopen the core");
    }

    void testCapabilityTruthAndHandshakeFreeze(tests::support::TestResult& result) {
        const auto configuredCapabilities = [](bool cppClientSdkBuilt, bool multipleTransportFamilies) {
            Backend backend;
            server::ServerCoreOptions options;
            if (cppClientSdkBuilt) {
                options.implementedCapabilities.push_back(frontend::FrontendCapability::CppClientSdk);
            }
            server::ServerCore core(backend, std::move(options));
            core.start();
            core.declareTransportFamily(frontend::FrontendTransportKind::Unix);
            if (multipleTransportFamilies) {
                core.declareTransportFamily(frontend::FrontendTransportKind::WebSocket);
            }
            return core.implementedCapabilities();
        };
        const std::vector<frontend::FrontendCapability> noSdkSingle = configuredCapabilities(false, false);
        const std::vector<frontend::FrontendCapability> noSdkMultiple = configuredCapabilities(false, true);
        const std::vector<frontend::FrontendCapability> sdkSingle = configuredCapabilities(true, false);
        const std::vector<frontend::FrontendCapability> sdkMultiple = configuredCapabilities(true, true);
        result.expectTrue(noSdkSingle.size() == 13 && !containsCapability(noSdkSingle, frontend::FrontendCapability::CppClientSdk) &&
                              !containsCapability(noSdkSingle, frontend::FrontendCapability::MultiTransport) &&
                              noSdkMultiple.size() == 14 &&
                              !containsCapability(noSdkMultiple, frontend::FrontendCapability::CppClientSdk) &&
                              containsCapability(noSdkMultiple, frontend::FrontendCapability::MultiTransport) && sdkSingle.size() == 14 &&
                              containsCapability(sdkSingle, frontend::FrontendCapability::CppClientSdk) &&
                              !containsCapability(sdkSingle, frontend::FrontendCapability::MultiTransport) && sdkMultiple.size() == 15 &&
                              containsCapability(sdkMultiple, frontend::FrontendCapability::CppClientSdk) &&
                              containsCapability(sdkMultiple, frontend::FrontendCapability::MultiTransport),
                          "ServerCore exercises all four SDK on/off and one/two-family capability-truth cells");

        Backend backend;
        server::ServerCoreOptions options;
        options.authenticator = authenticate;
        options.implementedCapabilities.push_back(frontend::FrontendCapability::CppClientSdk);
        options.maximumInboundMessageBytes = 768U * 1024U;
        server::ServerCore core(backend, std::move(options));
        core.start();
        core.declareTransportFamily(frontend::FrontendTransportKind::Unix);
        core.declareTransportFamily(frontend::FrontendTransportKind::WebSocket);

        std::vector<frontend::ServerMessage> firstMessages;
        const auto callbacks = [](std::vector<frontend::ServerMessage>& messages) {
            return server::ConnectionCallbacks{[&messages](server::SerializedServerMessage outbound) {
                                                   messages.push_back(std::move(outbound.message));
                                                   return true;
                                               },
                                               [](const server::ConnectionClose&) {
                                               }};
        };
        const auto first = core.openConnection({}, callbacks(firstMessages));
        frontend::Hello hello;
        hello.capabilities = std::vector<frontend::FrontendCapability>{frontend::FrontendCapability::MethodDiscovery};
        const bool firstAccepted = first && core.receive(*first, frontend::ClientMessage{hello}).accepted();
        const auto* firstWelcome = !firstMessages.empty() ? std::get_if<frontend::Welcome>(&firstMessages.front()) : nullptr;
        const std::optional<frontend::CapabilityAdvertisement> frozenAdvertisement =
            firstWelcome ? firstWelcome->capabilities : std::nullopt;
        result.expectTrue(firstWelcome &&
                              firstWelcome->maximumInboundMessageBytes == std::optional<std::uint64_t>{768U * 1024U},
                          "Welcome advertises the effective per-connection frontend ingress limit");

        core.withdrawTransportFamily(frontend::FrontendTransportKind::WebSocket);
        const std::vector<frontend::FrontendCapability> reduced = core.implementedCapabilities();
        std::vector<frontend::ServerMessage> secondMessages;
        const auto second = core.openConnection({}, callbacks(secondMessages));
        const bool secondAccepted = second && core.receive(*second, frontend::ClientMessage{hello}).accepted();
        const auto* retainedWelcome = !firstMessages.empty() ? std::get_if<frontend::Welcome>(&firstMessages.front()) : nullptr;
        const auto* secondWelcome = !secondMessages.empty() ? std::get_if<frontend::Welcome>(&secondMessages.front()) : nullptr;

        result.expectTrue(
            firstAccepted && frozenAdvertisement &&
                containsCapability(frozenAdvertisement->implemented, frontend::FrontendCapability::MultiTransport) && retainedWelcome &&
                retainedWelcome->capabilities == frozenAdvertisement &&
                !containsCapability(reduced, frontend::FrontendCapability::MultiTransport) && secondAccepted && secondWelcome &&
                secondWelcome->capabilities &&
                !containsCapability(secondWelcome->capabilities->implemented, frontend::FrontendCapability::MultiTransport),
            "capability truth is captured per Hello: a later topology change affects a new Welcome but cannot mutate the issued Welcome");
    }

    void testExceptionBoundaries(tests::support::TestResult& result) {
        Backend schedulerBackend;
        server::ServerCoreOptions schedulerOptions;
        schedulerOptions.authenticator = authenticate;
        schedulerOptions.scheduler = [](std::function<void()>) {
            throw std::runtime_error("injected scheduler failure");
        };
        server::ServerCore schedulerCore(schedulerBackend, std::move(schedulerOptions));
        schedulerCore.start();
        std::vector<frontend::ServerMessage> messages;
        const auto scheduled = schedulerCore.openConnection({}, {[&messages](server::SerializedServerMessage outbound) {
                                                                      messages.push_back(std::move(outbound.message));
                                                                      return true;
                                                                  },
                                                                  [](const server::ConnectionClose&) {
                                                                  }});
        result.expectTrue(scheduled && schedulerCore.receive(*scheduled, frontend::ClientMessage{frontend::Hello{}}).accepted() &&
                              messages.size() == 4 && std::holds_alternative<frontend::EventBatch>(messages.back()),
                          "a throwing scheduler is contained and delivery falls back deterministically to the inline turn");

        Backend snapshotBackend;
        snapshotBackend.throwSnapshot = true;
        server::ServerCoreOptions snapshotOptions;
        snapshotOptions.authenticator = authenticate;
        server::ServerCore snapshotCore(snapshotBackend, std::move(snapshotOptions));
        snapshotCore.start();
        std::vector<frontend::ServerMessage> snapshotMessages;
        std::vector<server::ConnectionClose> snapshotCloses;
        std::size_t snapshotMessageCountAtClose = 0;
        const auto snapshotConnection =
            snapshotCore.openConnection({}, {[&snapshotMessages](server::SerializedServerMessage outbound) {
                                                  snapshotMessages.push_back(std::move(outbound.message));
                                                  return true;
                                              },
                                              [&snapshotMessages, &snapshotCloses, &snapshotMessageCountAtClose](
                                                  const server::ConnectionClose& close) {
                                                  snapshotMessageCountAtClose = snapshotMessages.size();
                                                  snapshotCloses.push_back(close);
                                              }});
        const server::ReceiveResult failedSnapshot = snapshotCore.receive(*snapshotConnection, frontend::ClientMessage{frontend::Hello{}});
        const auto* terminalSnapshotError =
            snapshotMessages.size() == 2 ? std::get_if<frontend::ProtocolErrorMessage>(&snapshotMessages.back()) : nullptr;
        result.expectTrue(failedSnapshot.status == server::ReceiveStatus::Closing && failedSnapshot.error &&
                              failedSnapshot.error->code == frontend::ErrorCode::InternalError &&
                              failedSnapshot.error->closeConnection && snapshotMessages.size() == 2 &&
                              std::holds_alternative<frontend::Welcome>(snapshotMessages.front()) && terminalSnapshotError &&
                              terminalSnapshotError->code == frontend::ErrorCode::InternalError &&
                              terminalSnapshotError->message == "frontend initial synchronization failed" &&
                              terminalSnapshotError->closeConnection && snapshotMessageCountAtClose == 2 && snapshotCloses.size() == 1 &&
                              snapshotCloses.front().reason == "frontend initial synchronization failed" &&
                              snapshotCloses.front().protocolCode == frontend::ErrorCode::InternalError &&
                              !snapshotCloses.front().clean &&
                              snapshotCore.connectionCount() == 0,
                          "a throwing backend snapshot emits one terminal protocol error after Welcome and closes after it drains");

        Backend liveSnapshotBackend;
        server::ServerCoreOptions liveSnapshotOptions;
        liveSnapshotOptions.authenticator = authenticate;
        server::ServerCore liveSnapshotCore(liveSnapshotBackend, std::move(liveSnapshotOptions));
        liveSnapshotCore.start();
        std::vector<frontend::ServerMessage> liveSnapshotMessages;
        std::vector<server::ConnectionClose> liveSnapshotCloses;
        std::size_t liveSnapshotMessageCountAtClose = 0;
        const auto liveSnapshotConnection =
            liveSnapshotCore.openConnection({}, {[&liveSnapshotMessages](server::SerializedServerMessage outbound) {
                                                      liveSnapshotMessages.push_back(std::move(outbound.message));
                                                      return true;
                                                  },
                                                  [&liveSnapshotMessages, &liveSnapshotCloses, &liveSnapshotMessageCountAtClose](
                                                      const server::ConnectionClose& close) {
                                                      liveSnapshotMessageCountAtClose = liveSnapshotMessages.size();
                                                      liveSnapshotCloses.push_back(close);
                                                  }});
        const bool liveSnapshotReady =
            liveSnapshotConnection &&
            liveSnapshotCore.receive(*liveSnapshotConnection, frontend::ClientMessage{frontend::Hello{}}).accepted();
        liveSnapshotMessages.clear();
        model::CanonicalSnapshot unencodableSnapshot;
        model::ItemData unencodableItem{model::ItemIdentity{"oversized-live-item"}};
        unencodableItem.agentText = std::string(16'385, 'x');
        unencodableSnapshot.items.emplace_back(model::AgentMessageItem{std::move(unencodableItem)});
        const server::SnapshotPublishResult failedLiveSnapshot = liveSnapshotCore.publishSnapshot(std::move(unencodableSnapshot));
        const auto* terminalLiveSnapshotError = liveSnapshotMessages.size() == 1
                                                    ? std::get_if<frontend::ProtocolErrorMessage>(&liveSnapshotMessages.front())
                                                    : nullptr;
        result.expectTrue(liveSnapshotReady && failedLiveSnapshot.accepted && !failedLiveSnapshot.error &&
                              failedLiveSnapshot.recipientCount == 0 && terminalLiveSnapshotError &&
                              terminalLiveSnapshotError->code == frontend::ErrorCode::InternalError &&
                              terminalLiveSnapshotError->message ==
                                  "frontend live snapshot projection or queueing failed" &&
                              terminalLiveSnapshotError->closeConnection && liveSnapshotMessageCountAtClose == 1 &&
                              liveSnapshotCloses.size() == 1 &&
                              liveSnapshotCloses.front().protocolCode == frontend::ErrorCode::InternalError &&
                              !liveSnapshotCloses.front().clean && liveSnapshotCore.connectionCount() == 0,
                          "an unencodable live Snapshot emits one terminal protocol error before closing only its recipient");

        Backend protocolBackend;
        std::vector<std::function<void()>> protocolScheduled;
        server::ServerCoreOptions protocolOptions;
        protocolOptions.scheduler = [&protocolScheduled](std::function<void()> callback) {
            protocolScheduled.push_back(std::move(callback));
        };
        server::ServerCore protocolCore(protocolBackend, std::move(protocolOptions));
        protocolCore.start();
        std::vector<frontend::ServerMessage> protocolMessages;
        std::vector<server::ConnectionClose> protocolCloses;
        const auto protocolConnection = protocolCore.openConnection({},
                                                                    {[&protocolMessages](server::SerializedServerMessage outbound) {
                                                                         protocolMessages.push_back(std::move(outbound.message));
                                                                         return true;
                                                                     },
                                                                     [&protocolCloses](const server::ConnectionClose& close) {
                                                                         protocolCloses.push_back(close);
                                                                     }});
        const frontend::Json wrongProtocol{{"protocol", "not-the-frontend-protocol"},
                                           {"version", frontend::ProtocolVersion},
                                           {"kind", frontend::kind::Hello}};
        const server::ReceiveResult protocolFailure = protocolCore.receive(*protocolConnection, wrongProtocol);
        if (!protocolScheduled.empty()) {
            protocolScheduled.front()();
        }
        result.expectTrue(protocolFailure.status == server::ReceiveStatus::Closing && protocolMessages.size() == 1 &&
                              std::holds_alternative<frontend::ProtocolErrorMessage>(protocolMessages.front()) &&
                              protocolCloses.size() == 1 &&
                              protocolCloses.front().reason == "frontend protocol requested connection close" &&
                              protocolCloses.front().protocolCode == frontend::ErrorCode::WrongProtocol,
                          "a closing protocol error drains before the frozen constant close callback reason");
    }

    void testExplicitReentrancy(tests::support::TestResult& result) {
        Backend authenticatorBackend;
        server::ServerCore* authenticatorCorePointer = nullptr;
        std::optional<server::ConnectionIdentity> authenticatorIdentity;
        server::ReceiveResult nestedAuthenticatorResult;
        std::size_t authenticatorCloses = 0;
        server::ServerCoreOptions authenticatorOptions;
        authenticatorOptions.scheduler = [](std::function<void()>) {
        };
        authenticatorOptions.authenticator = [&](const frontend::FrontendPeerContext& peer,
                                                 const frontend::AuthenticationCredential& credential) {
            if (authenticatorCorePointer && authenticatorIdentity) {
                nestedAuthenticatorResult = authenticatorCorePointer->receive(*authenticatorIdentity, wrongProtocolMessage());
            }
            return authenticate(peer, credential);
        };
        server::ServerCore authenticatorCore(authenticatorBackend, std::move(authenticatorOptions));
        authenticatorCorePointer = &authenticatorCore;
        authenticatorCore.start();
        authenticatorIdentity = authenticatorCore.openConnection({}, {[](server::SerializedServerMessage) {
                                                                            return true;
                                                                        },
                                                                        [&](const server::ConnectionClose&) {
                                                                            ++authenticatorCloses;
                                                                        }});
        const server::ReceiveResult authenticatorResult =
            authenticatorIdentity
                ? authenticatorCore.receive(*authenticatorIdentity, frontend::ClientMessage{frontend::Hello{}})
                : server::ReceiveResult{server::ReceiveStatus::UnknownConnection, std::nullopt};
        const bool authenticatorStopped =
            authenticatorIdentity && nestedAuthenticatorResult.status == server::ReceiveStatus::Closing &&
            authenticatorResult.status == server::ReceiveStatus::Closed && !authenticatorCore.helloComplete(*authenticatorIdentity) &&
            authenticatorBackend.openedSessions.empty() && authenticatorBackend.closedSessions.empty() && authenticatorCloses == 0;
        authenticatorCore.flush();
        result.expectTrue(authenticatorStopped && authenticatorCloses == 1 && authenticatorCore.connectionCount() == 0,
                          "an authenticator that reentrantly marks the connection Closing invalidates the awaiting-Hello continuation");

        Backend cancellationBackend;
        server::ServerCore* cancellationCorePointer = nullptr;
        std::optional<server::ConnectionIdentity> cancellationIdentity;
        server::ReceiveResult nestedCancellationResult;
        std::size_t cancellationCloses = 0;
        server::ServerCoreOptions cancellationOptions;
        cancellationOptions.authenticator = authenticate;
        cancellationOptions.scheduler = [](std::function<void()>) {
        };
        cancellationOptions.timerScheduler = [&](std::uint64_t, std::function<void()>) {
            return [&] {
                if (cancellationCorePointer && cancellationIdentity) {
                    nestedCancellationResult = cancellationCorePointer->receive(*cancellationIdentity, wrongProtocolMessage());
                }
            };
        };
        server::ServerCore cancellationCore(cancellationBackend, std::move(cancellationOptions));
        cancellationCorePointer = &cancellationCore;
        cancellationCore.start();
        cancellationIdentity = cancellationCore.openConnection({}, {[](server::SerializedServerMessage) {
                                                                          return true;
                                                                      },
                                                                      [&](const server::ConnectionClose&) {
                                                                          ++cancellationCloses;
                                                                      }});
        const server::ReceiveResult cancellationResult =
            cancellationIdentity
                ? cancellationCore.receive(*cancellationIdentity, frontend::ClientMessage{frontend::Hello{}})
                : server::ReceiveResult{server::ReceiveStatus::UnknownConnection, std::nullopt};
        const bool cancellationStopped =
            cancellationIdentity && nestedCancellationResult.status == server::ReceiveStatus::Closing &&
            cancellationResult.status == server::ReceiveStatus::Closed && !cancellationCore.helloComplete(*cancellationIdentity) &&
            cancellationBackend.openedSessions.empty() && cancellationBackend.closedSessions.empty() && cancellationCloses == 0;
        cancellationCore.flush();
        result.expectTrue(cancellationStopped && cancellationCloses == 1 && cancellationCore.connectionCount() == 0,
                          "timer cancellation reentry cannot continue from awaiting Hello into an unannounced backend session");

        Backend immediateBackend;
        std::size_t immediateSchedules = 0;
        server::ServerCoreOptions immediateOptions;
        immediateOptions.authenticator = authenticate;
        immediateOptions.scheduler = [&immediateSchedules](std::function<void()> callback) {
            ++immediateSchedules;
            callback();
        };
        server::ServerCore immediate(immediateBackend, std::move(immediateOptions));
        immediate.start();
        std::vector<frontend::ServerMessage> immediateMessages;
        const auto immediateConnection = immediate.openConnection({}, {[&immediateMessages](server::SerializedServerMessage outbound) {
                                                                           immediateMessages.push_back(std::move(outbound.message));
                                                                           return true;
                                                                       },
                                                                       [](const server::ConnectionClose&) {
                                                                       }});
        const bool immediateHello = immediateConnection &&
                                    immediate.receive(*immediateConnection, frontend::ClientMessage{frontend::Hello{}}).accepted();
        result.expectTrue(immediateHello && immediateSchedules > 0 && immediateMessages.size() == 4 &&
                              immediate.connectionOpen(*immediateConnection),
                          "a scheduler that executes immediately completes one safe outer Hello delivery turn");

        Backend immediateTimerBackend;
        std::size_t immediateTimerCloses = 0;
        std::size_t immediateTimerCancellations = 0;
        server::ServerCoreOptions immediateTimerOptions;
        immediateTimerOptions.timerScheduler = [&immediateTimerCancellations](std::uint64_t, std::function<void()> callback) {
            callback();
            return [&immediateTimerCancellations] {
                ++immediateTimerCancellations;
            };
        };
        server::ServerCore immediateTimer(immediateTimerBackend, std::move(immediateTimerOptions));
        immediateTimer.start();
        const auto timedOut = immediateTimer.openConnection({}, {[](server::SerializedServerMessage) {
                                                                      return true;
                                                                  },
                                                                  [&immediateTimerCloses](const server::ConnectionClose&) {
                                                                      ++immediateTimerCloses;
                                                                  }});
        result.expectTrue(!timedOut && immediateTimer.connectionCount() == 0 && immediateTimerCloses == 1 &&
                              immediateTimerCancellations == 1,
                          "a timer scheduler that fires immediately closes and cancels one not-yet-returned connection safely");

        Backend selfBackend;
        server::ServerCoreOptions selfOptions;
        selfOptions.authenticator = authenticate;
        selfOptions.scheduler = [](std::function<void()> callback) {
            callback();
        };
        server::ServerCore selfClosing(selfBackend, std::move(selfOptions));
        selfClosing.start();
        std::optional<server::ConnectionIdentity> selfIdentity;
        std::size_t selfMessages = 0;
        std::size_t selfCloses = 0;
        bool closeSelf = false;
        selfIdentity = selfClosing.openConnection({}, {[&](server::SerializedServerMessage) {
                                                            ++selfMessages;
                                                            if (closeSelf) {
                                                                closeSelf = false;
                                                                selfClosing.closeConnection(*selfIdentity, "self-closing send callback");
                                                            }
                                                            return true;
                                                        },
                                                        [&](const server::ConnectionClose&) {
                                                            ++selfCloses;
                                                        }});
        closeSelf = true;
        if (selfIdentity) {
            static_cast<void>(selfClosing.receive(*selfIdentity, frontend::ClientMessage{frontend::Hello{}}));
        }
        result.expectTrue(selfIdentity && selfMessages == 1 && selfCloses == 1 && !selfClosing.connectionOpen(*selfIdentity),
                          "a Send callback may close its own connection without a later callback from the invalidated delivery");

        Backend peerBackend;
        bool closePeer = false;
        std::vector<frontend::ServerMessage> firstMessages;
        std::size_t secondMessages = 0;
        std::size_t secondCloses = 0;
        server::ServerCoreOptions peerOptions;
        peerOptions.authenticator = authenticate;
        peerOptions.scheduler = [](std::function<void()> callback) {
            callback();
        };
        server::ServerCore peerClosing(peerBackend, std::move(peerOptions));
        peerClosing.start();
        std::optional<server::ConnectionIdentity> secondIdentity;
        const auto firstIdentity = peerClosing.openConnection({}, {[&](server::SerializedServerMessage outbound) {
                                                                        firstMessages.push_back(std::move(outbound.message));
                                                                        if (closePeer) {
                                                                            closePeer = false;
                                                                            peerClosing.closeConnection(*secondIdentity,
                                                                                                        "peer-closing send callback");
                                                                        }
                                                                        return true;
                                                                    },
                                                                    [](const server::ConnectionClose&) {
                                                                    }});
        secondIdentity = peerClosing.openConnection({}, {[&](server::SerializedServerMessage) {
                                                                ++secondMessages;
                                                                return true;
                                                            },
                                                            [&](const server::ConnectionClose&) {
                                                                ++secondCloses;
                                                            }});
        const bool peersReady = firstIdentity && secondIdentity &&
                                peerClosing.receive(*firstIdentity, frontend::ClientMessage{frontend::Hello{}}).accepted() &&
                                peerClosing.receive(*secondIdentity, frontend::ClientMessage{frontend::Hello{}}).accepted();
        firstMessages.clear();
        secondMessages = 0;
        closePeer = true;
        const server::PublishResult peerPublished = peerClosing.publishGroup(providerOccurrence("903"));
        const auto* publishedBatch = firstMessages.size() > 0 ? std::get_if<frontend::EventBatch>(&firstMessages[0]) : nullptr;
        const auto* sessionBatch = firstMessages.size() > 1 ? std::get_if<frontend::EventBatch>(&firstMessages[1]) : nullptr;
        result.expectTrue(peersReady && peerPublished.accepted && firstMessages.size() == 2 && publishedBatch && sessionBatch &&
                              publishedBatch->fromSequence == peerPublished.sequence.protocolValue() &&
                              sessionBatch->fromSequence == peerClosing.currentSequence().protocolValue() &&
                              sessionBatch->fromSequence > publishedBatch->toSequence && secondMessages == 0 && secondCloses == 1 &&
                              firstIdentity && peerClosing.connectionOpen(*firstIdentity) &&
                              !peerClosing.connectionOpen(*secondIdentity),
                          "a Send callback may close another connection before its turn, then receives the distinct deferred session update");

        Backend closedBackend;
        server::ServerCore closedReentry(closedBackend);
        closedReentry.start();
        std::optional<server::ConnectionIdentity> closedFirst;
        std::optional<server::ConnectionIdentity> closedSecond;
        bool removedBeforeCallback = false;
        std::size_t firstClosedCallbacks = 0;
        std::size_t secondClosedCallbacks = 0;
        closedFirst = closedReentry.openConnection({}, {[](server::SerializedServerMessage) {
                                                             return true;
                                                         },
                                                         [&](const server::ConnectionClose&) {
                                                             ++firstClosedCallbacks;
                                                             removedBeforeCallback = !closedReentry.connectionOpen(*closedFirst);
                                                             closedReentry.closeConnection(*closedSecond, "closed callback reentry");
                                                         }});
        closedSecond = closedReentry.openConnection({}, {[](server::SerializedServerMessage) {
                                                              return true;
                                                          },
                                                          [&](const server::ConnectionClose&) {
                                                              ++secondClosedCallbacks;
                                                          }});
        if (closedFirst) {
            closedReentry.closeConnection(*closedFirst, "outer close");
        }
        result.expectTrue(closedFirst && closedSecond && removedBeforeCallback && firstClosedCallbacks == 1 &&
                              secondClosedCallbacks == 1 && closedReentry.connectionCount() == 0,
                          "onClosed observes its connection removed and may re-enter to close another connection exactly once");

        Backend sessionBackend;
        server::ServerCoreOptions sessionOptions;
        sessionOptions.authenticator = authenticate;
        sessionOptions.scheduler = [](std::function<void()>) {
        };
        server::ServerCore sessionReentry(sessionBackend, std::move(sessionOptions));
        sessionReentry.start();
        std::size_t sessionCloses = 0;
        const auto sessionIdentity = sessionReentry.openConnection({}, {[](server::SerializedServerMessage) {
                                                                            return true;
                                                                        },
                                                                        [&](const server::ConnectionClose&) {
                                                                            ++sessionCloses;
                                                                        }});
        sessionBackend.onSessionOpened = [&] {
            sessionReentry.closeConnection(*sessionIdentity, "session-open callback reentry");
        };
        const server::ReceiveResult sessionResult = sessionIdentity
                                                        ? sessionReentry.receive(*sessionIdentity,
                                                                                 frontend::ClientMessage{frontend::Hello{}})
                                                        : server::ReceiveResult{server::ReceiveStatus::UnknownConnection, std::nullopt};
        result.expectTrue(sessionIdentity && sessionResult.status == server::ReceiveStatus::Closed && sessionCloses == 1 &&
                              !sessionReentry.connectionOpen(*sessionIdentity) && sessionBackend.openedSessions.size() == 1 &&
                              sessionBackend.closedSessions.size() == 1,
                          "backend session-open reentry invalidates the connection and stops the old Hello continuation");

        Backend prematureCommandBackend;
        server::ServerCoreOptions prematureCommandOptions;
        prematureCommandOptions.authenticator = [](const frontend::FrontendPeerContext&,
                                                     const frontend::AuthenticationCredential&) {
            frontend::FrontendPrincipal principal;
            principal.id = "opening-session-principal";
            principal.profile = "test";
            principal.scopes.assign(frontend::LocalTrustedScopes.begin(), frontend::LocalTrustedScopes.end());
            return frontend::AuthenticationResult{frontend::AuthenticationSuccess{std::move(principal)}};
        };
        prematureCommandOptions.scheduler = [](std::function<void()>) {
        };
        server::ServerCore prematureCommandCore(prematureCommandBackend, std::move(prematureCommandOptions));
        prematureCommandCore.start();
        std::vector<frontend::ServerMessage> prematureMessages;
        std::size_t prematureCloses = 0;
        const auto prematureIdentity = prematureCommandCore.openConnection(
            {},
            {[&](server::SerializedServerMessage outbound) {
                 prematureMessages.push_back(std::move(outbound.message));
                 return true;
             },
             [&](const server::ConnectionClose&) {
                 ++prematureCloses;
             }});
        server::ReceiveResult prematureNested;
        prematureCommandBackend.onSessionOpened = [&] {
            generated::DefinedCommand acquire{
                "premature-controller-acquire",
                generated::makeParameters(generated::MethodId::ControllerAcquire, frontend::Json::object())};
            prematureNested = prematureCommandCore.receiveDefinedCommand(*prematureIdentity, acquire);
        };
        const server::ReceiveResult prematureOuter =
            prematureIdentity
                ? prematureCommandCore.receive(*prematureIdentity, frontend::ClientMessage{frontend::Hello{}})
                : server::ReceiveResult{server::ReceiveStatus::UnknownConnection, std::nullopt};
        const bool noPrematureWelcome = prematureNested.status == server::ReceiveStatus::Closing &&
                                        prematureOuter.status == server::ReceiveStatus::Closed &&
                                        !prematureCommandCore.helloComplete(*prematureIdentity) &&
                                        !prematureCommandCore.currentController() && prematureMessages.empty();
        prematureCommandCore.flush();
        result.expectTrue(noPrematureWelcome && prematureMessages.size() == 1 &&
                              std::holds_alternative<frontend::ProtocolErrorMessage>(prematureMessages.front()) &&
                              prematureCloses == 1 && prematureCommandCore.connectionCount() == 0,
                          "backend session-open reentry cannot dispatch a command or enqueue a response before Welcome activation");

        Backend controllerBackend;
        server::ServerCoreOptions controllerOptions;
        controllerOptions.authenticator = [](const frontend::FrontendPeerContext&, const frontend::AuthenticationCredential&) {
            frontend::FrontendPrincipal principal;
            principal.id = "controller-reentry-principal";
            principal.profile = "test";
            principal.scopes.assign(frontend::LocalTrustedScopes.begin(), frontend::LocalTrustedScopes.end());
            return frontend::AuthenticationResult{frontend::AuthenticationSuccess{std::move(principal)}};
        };
        controllerOptions.scheduler = [](std::function<void()>) {
        };
        server::ServerCore controllerReentry(controllerBackend, std::move(controllerOptions));
        controllerReentry.start();
        std::size_t controllerCloses = 0;
        const auto controllerIdentity = controllerReentry.openConnection({}, {[](server::SerializedServerMessage) {
                                                                                   return true;
                                                                               },
                                                                               [&](const server::ConnectionClose&) {
                                                                                   ++controllerCloses;
                                                                               }});
        const bool controllerReady = controllerIdentity &&
                                     controllerReentry.receive(*controllerIdentity, frontend::ClientMessage{frontend::Hello{}}).accepted();
        controllerBackend.onControllerChanged = [&] {
            controllerReentry.closeConnection(*controllerIdentity, "controller callback reentry");
        };
        generated::DefinedCommand acquire{
            "reentrant-controller-acquire",
            generated::makeParameters(generated::MethodId::ControllerAcquire, frontend::Json::object())};
        const server::ReceiveResult controllerResult =
            controllerIdentity ? controllerReentry.receiveDefinedCommand(*controllerIdentity, acquire)
                               : server::ReceiveResult{server::ReceiveStatus::UnknownConnection, std::nullopt};
        result.expectTrue(controllerReady && controllerResult.status == server::ReceiveStatus::Closed && controllerCloses == 1 &&
                              !controllerReentry.currentController() && !controllerReentry.connectionOpen(*controllerIdentity),
                          "backend controller-change reentry invalidates the connection and stops the old command continuation");

        Backend releaseBackend;
        server::ServerCoreOptions releaseOptions;
        releaseOptions.authenticator = [](const frontend::FrontendPeerContext&, const frontend::AuthenticationCredential&) {
            frontend::FrontendPrincipal principal;
            principal.id = "release-reentry-principal";
            principal.profile = "test";
            principal.scopes.assign(frontend::LocalTrustedScopes.begin(), frontend::LocalTrustedScopes.end());
            return frontend::AuthenticationResult{frontend::AuthenticationSuccess{std::move(principal)}};
        };
        releaseOptions.scheduler = [](std::function<void()>) {
        };
        server::ServerCore releaseCore(releaseBackend, std::move(releaseOptions));
        releaseCore.start();
        std::vector<frontend::ServerMessage> releaseMessages;
        const auto releaseIdentity = releaseCore.openConnection({}, {[&](server::SerializedServerMessage outbound) {
                                                                          releaseMessages.push_back(std::move(outbound.message));
                                                                          return true;
                                                                      },
                                                                      [](const server::ConnectionClose&) {
                                                                      }});
        const bool releaseReady = releaseIdentity &&
                                  releaseCore.receive(*releaseIdentity, frontend::ClientMessage{frontend::Hello{}}).accepted();
        generated::DefinedCommand initialAcquire{
            "initial-controller-acquire",
            generated::makeParameters(generated::MethodId::ControllerAcquire, frontend::Json::object())};
        const bool initiallyAcquired = releaseIdentity && releaseCore.receiveDefinedCommand(*releaseIdentity, initialAcquire).accepted();
        if (releaseIdentity) {
            releaseCore.flushConnection(*releaseIdentity);
        }
        releaseMessages.clear();
        server::ReceiveResult nestedReacquire;
        releaseBackend.onControllerChanged = [&] {
            generated::DefinedCommand reacquire{
                "nested-controller-reacquire",
                generated::makeParameters(generated::MethodId::ControllerAcquire, frontend::Json::object())};
            nestedReacquire = releaseCore.receiveDefinedCommand(*releaseIdentity, reacquire);
        };
        generated::DefinedCommand release{
            "outer-controller-release",
            generated::makeParameters(generated::MethodId::ControllerRelease, frontend::Json::object())};
        const server::ReceiveResult releaseResult =
            releaseIdentity ? releaseCore.receiveDefinedCommand(*releaseIdentity, release)
                            : server::ReceiveResult{server::ReceiveStatus::UnknownConnection, std::nullopt};
        if (releaseIdentity) {
            releaseCore.flushConnection(*releaseIdentity);
        }
        const auto releaseResponseIterator = std::find_if(releaseMessages.begin(), releaseMessages.end(), [](const auto& message) {
            const auto* response = std::get_if<frontend::Response>(&message);
            return response && response->requestId == "outer-controller-release";
        });
        const auto* releaseResponse =
            releaseResponseIterator != releaseMessages.end() ? std::get_if<frontend::Response>(&*releaseResponseIterator) : nullptr;
        result.expectTrue(releaseReady && initiallyAcquired && nestedReacquire.status == server::ReceiveStatus::Rejected &&
                              releaseResult.status == server::ReceiveStatus::Accepted && releaseResponse && releaseResponse->ok &&
                              releaseIdentity && !releaseCore.currentController(),
                          "an in-flight release rejects reentrant acquisition and commits one backend-confirmed observer transition");

        Backend policyBackend;
        server::ServerCore* policyCorePointer = nullptr;
        std::optional<server::ConnectionIdentity> policyIdentity;
        server::ReceiveResult nestedPolicyResult;
        std::size_t policyCloses = 0;
        server::ServerCoreOptions policyOptions;
        policyOptions.enableFilesystemReadMethods = true;
        policyOptions.authenticator = [](const frontend::FrontendPeerContext&, const frontend::AuthenticationCredential&) {
            frontend::FrontendPrincipal principal;
            principal.id = "policy-reentry-principal";
            principal.profile = "test";
            principal.scopes = {frontend::FrontendScope::Observe, frontend::FrontendScope::FilesystemRead};
            return frontend::AuthenticationResult{frontend::AuthenticationSuccess{std::move(principal)}};
        };
        policyOptions.scheduler = [](std::function<void()>) {
        };
        policyOptions.filesystemReadPolicy = [&](const frontend::FrontendPrincipal&, std::string_view, const frontend::Json&) {
            if (policyCorePointer && policyIdentity) {
                nestedPolicyResult = policyCorePointer->receive(*policyIdentity, wrongProtocolMessage());
            }
            return true;
        };
        server::ServerCore policyCore(policyBackend, std::move(policyOptions));
        policyCorePointer = &policyCore;
        policyCore.start();
        policyIdentity = policyCore.openConnection({}, {[](server::SerializedServerMessage) {
                                                              return true;
                                                          },
                                                          [&](const server::ConnectionClose&) {
                                                              ++policyCloses;
                                                          }});
        const bool policyReady = policyIdentity &&
                                 policyCore.receive(*policyIdentity, frontend::ClientMessage{frontend::Hello{}}).accepted();
        policyCore.flush();
        generated::DefinedCommand filesystemRead{
            "policy-reentry",
            generated::makeParameters(generated::MethodId::FsGetMetadata, frontend::Json{{"path", "x"}})};
        const server::ReceiveResult policyResult =
            policyIdentity ? policyCore.receiveDefinedCommand(*policyIdentity, filesystemRead)
                           : server::ReceiveResult{server::ReceiveStatus::UnknownConnection, std::nullopt};
        const bool policyStopped = policyReady && nestedPolicyResult.status == server::ReceiveStatus::Closing &&
                                   policyResult.status == server::ReceiveStatus::Closed && policyBackend.submissionCount == 0 &&
                                   policyCloses == 0;
        policyCore.flush();
        result.expectTrue(policyStopped && policyCloses == 1 && policyCore.connectionCount() == 0,
                          "invocation-policy reentry that changes lifecycle stops before backend submission or a stale denial response");

        Backend staleTimerBackend;
        std::shared_ptr<Timer> staleTimer;
        std::size_t staleTimerCloses = 0;
        server::ServerCoreOptions staleTimerOptions;
        staleTimerOptions.timerScheduler = [&staleTimer](std::uint64_t, std::function<void()> callback) {
            staleTimer = std::make_shared<Timer>(Timer{std::move(callback), false});
            return [staleTimer] {
                staleTimer->cancelled = true;
            };
        };
        server::ServerCore staleTimerCore(staleTimerBackend, std::move(staleTimerOptions));
        staleTimerCore.start();
        const auto staleIdentity = staleTimerCore.openConnection({}, {[](server::SerializedServerMessage) {
                                                                          return true;
                                                                      },
                                                                      [&](const server::ConnectionClose&) {
                                                                          ++staleTimerCloses;
                                                                      }});
        if (staleIdentity) {
            staleTimerCore.closeConnection(*staleIdentity, "close before stale timer");
        }
        if (staleTimer) {
            staleTimer->callback();
        }
        result.expectTrue(staleIdentity && staleTimer && staleTimer->cancelled && staleTimerCloses == 1 &&
                              staleTimerCore.connectionCount() == 0,
                          "a cancelled timer callback after connection close is harmless and cannot close twice");

        Backend serverTimerBackend;
        std::shared_ptr<Timer> serverTimer;
        std::size_t serverTimerCloses = 0;
        server::ServerCoreOptions serverTimerOptions;
        serverTimerOptions.timerScheduler = [&serverTimer](std::uint64_t, std::function<void()> callback) {
            serverTimer = std::make_shared<Timer>(Timer{std::move(callback), false});
            return [serverTimer] {
                serverTimer->cancelled = true;
            };
        };
        server::ServerCore serverTimerCore(serverTimerBackend, std::move(serverTimerOptions));
        serverTimerCore.start();
        const auto serverTimerIdentity = serverTimerCore.openConnection({}, {[](server::SerializedServerMessage) {
                                                                                  return true;
                                                                              },
                                                                              [&](const server::ConnectionClose&) {
                                                                                  ++serverTimerCloses;
                                                                              }});
        serverTimerCore.close("close before stale server timer");
        if (serverTimer) {
            serverTimer->callback();
        }
        result.expectTrue(serverTimerIdentity && serverTimer && serverTimer->cancelled && serverTimerCloses == 1 &&
                              !serverTimerCore.isOpen() && serverTimerCore.connectionCount() == 0,
                          "a handshake timer callback retained past terminal server close is generation-harmless");
    }

    void testFrozenSnapshotBarrier(tests::support::TestResult& result) {
        Backend backend;
        server::ServerCoreOptions options;
        options.authenticator = authenticate;
        options.maxOutboundMessagesPerConnection = 4;
        options.scheduler = [](std::function<void()>) {
        };
        server::ServerCore core(backend, std::move(options));
        core.start();

        std::vector<frontend::ServerMessage> firstMessages;
        std::vector<frontend::ServerMessage> secondMessages;
        std::size_t firstCloses = 0;
        std::optional<server::PublishResult> reentrantPublish;
        const auto first = core.openConnection({}, {[&](server::SerializedServerMessage outbound) {
                                                          firstMessages.push_back(std::move(outbound.message));
                                                          return true;
                                                      },
                                                      [&](const server::ConnectionClose&) {
                                                          ++firstCloses;
                                                          reentrantPublish = core.publishGroup(providerOccurrence("904"));
                                                      }});
        const auto second = core.openConnection({}, {[&](server::SerializedServerMessage outbound) {
                                                           secondMessages.push_back(std::move(outbound.message));
                                                           return true;
                                                       },
                                                       [](const server::ConnectionClose&) {
                                                       }});
        const bool firstReady = first && core.receive(*first, frontend::ClientMessage{frontend::Hello{}}).accepted();
        core.flush();
        const bool secondReady = second && core.receive(*second, frontend::ClientMessage{frontend::Hello{}}).accepted();
        core.flush();
        firstMessages.clear();
        secondMessages.clear();

        for (std::size_t index = 0; first && index < 4; ++index) {
            const frontend::Json unknownCommand{{"protocol", frontend::ProtocolIdentity},
                                                {"version", frontend::ProtocolVersion},
                                                {"kind", frontend::kind::Command},
                                                {"requestId", "queued-" + std::to_string(index)},
                                                {"method", "not.generated"},
                                                {"params", frontend::Json::object()}};
            static_cast<void>(core.receive(*first, unknownCommand));
        }
        const std::size_t firstQueuedBeforeSnapshot = first ? core.queuedMessages(*first) : 0;

        const server::SnapshotPublishResult published = core.publishSnapshot(backend.state);
        if (first) {
            core.flushConnection(*first);
        }
        if (second) {
            core.flushConnection(*second);
        }
        const frontend::Snapshot* deliveredSnapshot = nullptr;
        for (const frontend::ServerMessage& message : secondMessages) {
            if (const auto* snapshot = std::get_if<frontend::Snapshot>(&message)) {
                deliveredSnapshot = snapshot;
            }
        }
        result.expectTrue(firstReady && secondReady && firstQueuedBeforeSnapshot == 4 && firstCloses == 1 &&
                              reentrantPublish && reentrantPublish->accepted && published.accepted && !published.error &&
                              published.recipientCount == 1 && deliveredSnapshot &&
                              deliveredSnapshot->sequence == published.sequence.protocolValue() &&
                              reentrantPublish->sequence > published.sequence,
                          "one frozen live-Snapshot barrier and result survive an earlier recipient close callback that advances sequence");
    }

    void testClockAndHandshakeTimerReentrancy(tests::support::TestResult& result) {
        const auto clockCloseIsContained = [](std::size_t closeOnCall) {
            Backend backend;
            server::ServerCore* corePointer = nullptr;
            std::size_t clockCalls = 0;
            std::size_t timerSchedules = 0;
            server::ServerCoreOptions options;
            options.monotonicClockMs = [&] {
                ++clockCalls;
                if (clockCalls == closeOnCall && corePointer) {
                    corePointer->close("monotonic clock reentry");
                }
                return static_cast<std::uint64_t>(clockCalls);
            };
            options.timerScheduler = [&](std::uint64_t, std::function<void()>) {
                ++timerSchedules;
                return [] {
                };
            };
            server::ServerCore core(backend, std::move(options));
            corePointer = &core;
            core.start();
            const frontend::AuthenticationFailureCode recorded = core.recordPreAuthenticationFailure(
                frontend::FrontendPeerContext{}, frontend::AuthenticationFailureCode::AuthenticationFailed);
            return recorded == frontend::AuthenticationFailureCode::RateLimited && !core.isOpen() && timerSchedules == 0;
        };
        result.expectTrue(clockCloseIsContained(1) && clockCloseIsContained(2),
                          "monotonic-clock reentry cannot record or schedule authentication-failure state after terminal close");

        Backend backend;
        std::size_t timerSchedules = 0;
        std::size_t timerCancellations = 0;
        server::ServerCoreOptions options;
        options.authenticator = [](const frontend::FrontendPeerContext&, const frontend::AuthenticationCredential&) {
            frontend::FrontendPrincipal principal;
            principal.id = "timer-ownership-principal";
            principal.profile = "test";
            principal.scopes.assign(frontend::LocalTrustedScopes.begin(), frontend::LocalTrustedScopes.end());
            return frontend::AuthenticationResult{frontend::AuthenticationSuccess{std::move(principal)}};
        };
        options.scheduler = [](std::function<void()>) {
        };
        options.timerScheduler = [&](std::uint64_t, std::function<void()>) {
            ++timerSchedules;
            return [&] {
                ++timerCancellations;
            };
        };
        server::ServerCore core(backend, std::move(options));
        core.start();
        const auto first = core.openConnection({}, {[](server::SerializedServerMessage) {
                                                         return true;
                                                     },
                                                     [](const server::ConnectionClose&) {
                                                     }});
        const auto second = core.openConnection({}, {[](server::SerializedServerMessage) {
                                                          return true;
                                                      },
                                                      [](const server::ConnectionClose&) {
                                                      }});
        const bool synchronized = first && second &&
                                  core.receive(*first, frontend::ClientMessage{frontend::Hello{}}).accepted() &&
                                  core.receive(*second, frontend::ClientMessage{frontend::Hello{}}).accepted();
        generated::DefinedCommand acquire{
            "timer-owner-acquire",
            generated::makeParameters(generated::MethodId::ControllerAcquire, frontend::Json::object())};
        const bool acquired = first && core.receiveDefinedCommand(*first, acquire).accepted();
        const std::size_t cancellationsBeforeClose = timerCancellations;
        const std::optional<model::SessionIdentity> firstSession = first ? core.session(*first) : std::nullopt;
        if (first) {
            core.closeConnection(*first, "close authenticated controller");
        }
        const bool exactControllerHistory =
            firstSession && backend.controllerSessions.size() == 2 &&
            backend.controllerSessions[0] == firstSession->value() && backend.controllerSessions[1].empty();
        result.expectTrue(synchronized && acquired && timerSchedules == 2 && cancellationsBeforeClose == 2 &&
                              timerCancellations == cancellationsBeforeClose && exactControllerHistory,
                          "an authenticated controller owns no handshake cancellation that can reenter its close transition");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testLifecycle(result);
    testCapabilityTruthAndHandshakeFreeze(result);
    testExceptionBoundaries(result);
    testExplicitReentrancy(result);
    testFrozenSnapshotBarrier(result);
    testClockAndHandshakeTimerReentrancy(result);
    return result.processResult();
}
