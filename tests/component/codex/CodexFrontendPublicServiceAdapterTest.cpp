/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "CodexBackendTestSupport.h"
#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/FrontendService.h"
#include "ai/openai/codex/frontend/detail/FrontendServiceTestAccess.h"
#include "ai/openai/codex/frontend/internal/server/BackendCoreBridge.h"
#include "support/TestResult.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace backend = ai::openai::codex::backend;
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = ai::openai::codex::frontend::generated;
    namespace server = ai::openai::codex::frontend::internal::server;

    using FakeBackendCore = backend::BackendCore<tests::codex::FakeAppServerClient>;

    class ManualScheduler {
    public:
        void schedule(std::function<void()> callback) {
            callbacks.push_back(std::move(callback));
        }

        bool runOne() {
            if (callbacks.empty()) {
                return false;
            }
            std::function<void()> callback = std::move(callbacks.front());
            callbacks.pop_front();
            callback();
            return true;
        }

        [[nodiscard]] std::size_t pending() const noexcept {
            return callbacks.size();
        }

        void drain(std::size_t limit = 10'000) {
            std::size_t count = 0;
            while (!callbacks.empty()) {
                if (++count > limit) {
                    throw std::runtime_error("public service adapter scheduler did not quiesce");
                }
                static_cast<void>(runOne());
            }
        }

    private:
        std::deque<std::function<void()>> callbacks;
    };

    struct Observations {
        std::vector<frontend::ServerMessage> messages;
        std::vector<std::string> closes;
        bool outboundConversionValid = true;
        std::size_t reentrantQueries = 0;
    };

    frontend::FrontendPeerContext remotePeer(std::string address) {
        frontend::FrontendPeerContext peer;
        peer.transport = frontend::FrontendTransportKind::Ipv4;
        peer.loopback = true;
        peer.remoteAddress = std::move(address);
        return peer;
    }

    frontend::Hello hello(std::string token) {
        return {std::nullopt,
                frontend::Json::object(),
                std::vector<frontend::FrontendCapability>{frontend::FrontendCapability::DedicatedNotificationEvents},
                frontend::AuthenticationCredential{frontend::BearerCredential{std::move(token)}}};
    }

    frontend::ClientMessage command(std::string requestId, frontend::CommandParameters parameters) {
        return frontend::Command{std::move(requestId), std::move(parameters), frontend::Json::object(), frontend::Json::object()};
    }

    frontend::FrontendConnectionCallbacks callbacksFor(Observations& observations, frontend::FrontendService* service = nullptr) {
        return {[&observations, service](const frontend::OutboundMessage& outbound) {
                    const auto decoded = frontend::Codec::decodeServer(std::string_view(outbound.compactJson));
                    observations.outboundConversionValid = observations.outboundConversionValid && decoded &&
                                                           decoded.value() == outbound.message &&
                                                           outbound.serializedBytes == outbound.compactJson.size();
                    observations.messages.push_back(outbound.message);
                    if (service) {
                        static_cast<void>(service->connectionCount());
                        service->flush();
                        ++observations.reentrantQueries;
                    }
                    return true;
                },
                [&observations](const std::string& reason) {
                    observations.closes.push_back(reason);
                }};
    }

    const frontend::Response* response(const Observations& observations, const std::string& requestId) {
        for (const frontend::ServerMessage& message : observations.messages) {
            if (const auto* value = std::get_if<frontend::Response>(&message); value && value->requestId == requestId) {
                return value;
            }
        }
        return nullptr;
    }

    std::size_t countEvents(const Observations& observations, std::string_view type) {
        std::size_t count = 0;
        for (const frontend::ServerMessage& message : observations.messages) {
            if (const auto* batch = std::get_if<frontend::EventBatch>(&message)) {
                count += static_cast<std::size_t>(std::count_if(batch->events.begin(), batch->events.end(), [type](const auto& event) {
                    return event.type == type;
                }));
            }
        }
        return count;
    }

    const frontend::Snapshot* latestSnapshot(const Observations& observations, std::size_t begin = 0) {
        for (std::size_t index = observations.messages.size(); index > begin; --index) {
            if (const auto* snapshot = std::get_if<frontend::Snapshot>(&observations.messages[index - 1])) {
                return snapshot;
            }
        }
        return nullptr;
    }

    std::vector<std::string> sessionIds(const frontend::Json& state) {
        std::vector<std::string> ids;
        const auto sessions = state.find("sessions");
        if (sessions == state.end() || !sessions->is_array()) {
            return ids;
        }
        for (const frontend::Json& session : *sessions) {
            const auto id = session.find("sessionId");
            if (id != session.end() && id->is_string()) {
                ids.push_back(id->get<std::string>());
            }
        }
        return ids;
    }

    std::optional<std::string> controllerId(const frontend::Json& state) {
        const auto legacyController = state.find("controllerSessionId");
        if (legacyController != state.end() && legacyController->is_string()) {
            return legacyController->get<std::string>();
        }
        const auto controller = state.find("controller");
        if (controller == state.end() || !controller->is_object() || !controller->value("present", false)) {
            return std::nullopt;
        }
        const auto session = controller->find("controllerSessionId");
        return session != controller->end() && session->is_string() ? std::optional<std::string>{session->get<std::string>()}
                                                                    : std::nullopt;
    }

    bool containsIdentity(const std::vector<std::string>& identities, const std::optional<std::string>& identity) {
        return identity && std::find(identities.begin(), identities.end(), *identity) != identities.end();
    }

    std::optional<std::string> identityOutside(const std::vector<std::string>& identities, const std::vector<std::string>& excluded) {
        const auto found = std::find_if(identities.begin(), identities.end(), [&](const std::string& identity) {
            return std::find(excluded.begin(), excluded.end(), identity) == excluded.end();
        });
        return found == identities.end() ? std::nullopt : std::optional<std::string>{*found};
    }

    bool exactIdentitySet(std::vector<std::string> actual, std::vector<std::string> expected) {
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        return std::adjacent_find(actual.begin(), actual.end()) == actual.end() && actual == expected;
    }

    std::vector<frontend::FrontendEvent> eventsOfType(const Observations& observations, std::size_t begin, std::string_view type) {
        std::vector<frontend::FrontendEvent> events;
        for (std::size_t index = begin; index < observations.messages.size(); ++index) {
            const auto* batch = std::get_if<frontend::EventBatch>(&observations.messages[index]);
            if (!batch) {
                continue;
            }
            for (const frontend::FrontendEvent& event : batch->events) {
                if (event.type == type) {
                    events.push_back(event);
                }
            }
        }
        return events;
    }

    std::optional<std::string> eventControllerId(const frontend::FrontendEvent& event) {
        const auto controller = event.data.find("controller");
        return controller != event.data.end() && controller->is_object() ? controllerId(*controller) : controllerId(event.data);
    }

    std::vector<std::string> backendSessionIds(const backend::Snapshot& snapshot) {
        std::vector<std::string> identities;
        identities.reserve(snapshot.sessions.size());
        for (const backend::SessionSnapshot& session : snapshot.sessions) {
            identities.push_back(std::to_string(session.id.value()));
        }
        return identities;
    }

    void testPublicAdapter(tests::support::TestResult& result) {
        const auto transport = std::make_shared<tests::codex::FakeTransportState>();
        ManualScheduler scheduler;
        backend::BackendCoreOptions backendOptions;
        backendOptions.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        FakeBackendCore core(std::move(backendOptions), transport);

        std::size_t authenticationCalls = 0;
        std::size_t successfulAuthentications = 0;
        std::size_t expectedBackendSessionsBeforeAuthentication = 0;
        bool authenticationObservedExpectedBackendSessions = true;
        std::size_t timerCancellations = 0;
        frontend::FrontendServiceOptions options;
        options.journal = {23, 32U * 1024U, frontend::SequenceNumber{17}};
        options.batches = {7, 16U * 1024U};
        options.maxConnections = 4;
        options.maxUnauthenticatedConnections = 4;
        options.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        options.timerScheduler = [&timerCancellations](std::uint64_t, std::function<void()>) {
            return [&timerCancellations] {
                ++timerCancellations;
            };
        };
        options.authenticator = [&core,
                                 &authenticationCalls,
                                 &successfulAuthentications,
                                 &expectedBackendSessionsBeforeAuthentication,
                                 &authenticationObservedExpectedBackendSessions](
                                    const frontend::FrontendPeerContext&,
                                    const frontend::AuthenticationCredential& credential) -> frontend::AuthenticationResult {
            ++authenticationCalls;
            authenticationObservedExpectedBackendSessions = authenticationObservedExpectedBackendSessions &&
                                                            core.snapshot().sessions.size() == expectedBackendSessionsBeforeAuthentication;
            const auto* bearer = std::get_if<frontend::BearerCredential>(&credential);
            if (!bearer || bearer->token != "adapter-token") {
                return frontend::AuthenticationFailure{frontend::AuthenticationFailureCode::AuthenticationFailed};
            }
            ++successfulAuthentications;
            frontend::FrontendPrincipal principal;
            principal.id = "adapter-principal";
            principal.profile = "adapter-test";
            principal.scopes = {frontend::FrontendScope::Observe, frontend::FrontendScope::Control};
            return frontend::AuthenticationSuccess{std::move(principal)};
        };

        frontend::FrontendService service(core, options);
        service.declareTransportFamily(frontend::FrontendTransportKind::Ipv4);
        service.declareTransportFamily(frontend::FrontendTransportKind::WebSocket);
        service.withdrawTransportFamily(frontend::FrontendTransportKind::WebSocket);
        const auto transports = service.enabledTransportFamilies();
#if defined(AISUITE_CODEX_CPP_CLIENT_SDK_BUILT) && AISUITE_CODEX_CPP_CLIENT_SDK_BUILT
        constexpr bool CppClientSdkBuilt = true;
#else
        constexpr bool CppClientSdkBuilt = false;
#endif
        const std::vector<frontend::FrontendCapability> implementedCapabilities = service.implementedCapabilities();
        result.expectTrue(
            service.isOpen() && service.currentSequence() == frontend::SequenceNumber{17} && service.journalConfig() == options.journal &&
                service.batchConfig() == options.batches &&
                std::find(transports.begin(), transports.end(), frontend::FrontendTransportKind::Ipv4) != transports.end() &&
                std::find(transports.begin(), transports.end(), frontend::FrontendTransportKind::WebSocket) == transports.end() &&
                (std::find(implementedCapabilities.begin(), implementedCapabilities.end(), frontend::FrontendCapability::CppClientSdk) !=
                 implementedCapabilities.end()) == CppClientSdkBuilt,
            "the public wrapper converts options, product capability truth, and transport declarations");

        Observations rejected;
        frontend::FrontendConnection rejectedConnection =
            service.openConnection(remotePeer("127.0.0.1:41000"), callbacksFor(rejected, &service));
        result.expectTrue(core.snapshot().sessions.empty() && rejectedConnection.isOpen() &&
                              rejectedConnection.receive(frontend::ClientMessage{hello("wrong-token")}).status ==
                                  frontend::ConnectionReceiveStatus::Closing &&
                              core.snapshot().sessions.empty(),
                          "opening a physical connection and rejecting authentication creates no BackendCore session");
        scheduler.drain();
        result.expectTrue(authenticationCalls == 1 && authenticationObservedExpectedBackendSessions && !rejectedConnection.isOpen() &&
                              !rejected.closes.empty(),
                          "the public authentication callback runs before backend-session creation and terminal rejection is bounded");

        Observations first;
        frontend::FrontendConnection firstConnection = service.openConnection(remotePeer("127.0.0.1:41001"), callbacksFor(first, &service));
        frontend::FrontendPeerContext completedPeer = firstConnection.peer();
        completedPeer.origin = "https://adapter.test";
        result.expectTrue(firstConnection.updatePeerContext(completedPeer) && firstConnection.peer() == completedPeer &&
                              firstConnection.receive(frontend::ClientMessage{hello("adapter-token")}).accepted(),
                          "FrontendConnection delegates pre-Hello peer completion and typed receive to ServerCore");
        scheduler.drain();
        result.expectTrue(core.snapshot().sessions.size() == 1 && service.authenticatedConnectionCount() == 1 &&
                              service.unauthenticatedConnectionCount() == 0 && firstConnection.helloComplete() &&
                              firstConnection.sessionId().has_value() && firstConnection.principal().has_value() &&
                              firstConnection.principal()->id == "adapter-principal" && !firstConnection.updatePeerContext({}) &&
                              first.outboundConversionValid && first.reentrantQueries != 0 && timerCancellations == authenticationCalls,
                          "successful Hello creates one backend session and preserves public connection queries and serialized callbacks");

        Observations second;
        frontend::FrontendConnection secondConnection =
            service.openConnection(remotePeer("127.0.0.1:41002"), callbacksFor(second, &service));
        expectedBackendSessionsBeforeAuthentication = 1;
        result.expectTrue(secondConnection.receive(frontend::ClientMessage{hello("adapter-token")}).accepted(),
                          "a second authenticated frontend is admitted through the same public service");
        scheduler.drain();
        result.expectTrue(core.snapshot().sessions.size() == 2 && service.authenticatedConnectionCount() == 2,
                          "each authenticated frontend owns exactly one BackendCore command session");

        const std::size_t firstControllerBaseline = countEvents(first, "controller.updated");
        const std::size_t secondControllerBaseline = countEvents(second, "controller.updated");
        result.expectTrue(firstConnection.receive(command("adapter-acquire", frontend::ControllerAcquire{})).accepted() &&
                              core.snapshot().controller.has_value() && !service.currentController().has_value() &&
                              response(first, "adapter-acquire") == nullptr,
                          "BackendCore acquisition is submitted immediately while canonical frontend ownership awaits completion");
        scheduler.drain();
        const frontend::Response* acquired = response(first, "adapter-acquire");
        result.expectTrue(acquired && acquired->ok && service.currentController() == firstConnection.sessionId() &&
                              countEvents(first, "controller.updated") == firstControllerBaseline + 1 &&
                              countEvents(second, "controller.updated") == secondControllerBaseline + 1,
                          "successful backend completion commits controller ownership once through the shared observer bridge");

        const std::size_t firstConflictBaseline = countEvents(first, "controller.updated");
        const std::size_t secondConflictBaseline = countEvents(second, "controller.updated");
        result.expectTrue(secondConnection.receive(command("adapter-conflict", frontend::ControllerAcquire{})).accepted() &&
                              service.currentController() == firstConnection.sessionId() && response(second, "adapter-conflict") == nullptr,
                          "a known competing acquisition queues a local conflict without changing canonical frontend ownership");
        scheduler.drain();
        const frontend::Response* conflict = response(second, "adapter-conflict");
        result.expectTrue(conflict && !conflict->ok && conflict->error && conflict->error->code == frontend::ErrorCode::Conflict &&
                              service.currentController() == firstConnection.sessionId() &&
                              countEvents(first, "controller.updated") == firstConflictBaseline &&
                              countEvents(second, "controller.updated") == secondConflictBaseline,
                          "local controller-conflict prevalidation preserves the controller and emits no duplicate transition");

        result.expectTrue(firstConnection.receive(command("adapter-release", frontend::ControllerRelease{})).accepted() &&
                              !core.snapshot().controller.has_value() && service.currentController() == firstConnection.sessionId(),
                          "backend release precedes, but does not pre-commit, the public controller transition");
        scheduler.drain();
        const frontend::Response* released = response(first, "adapter-release");
        result.expectTrue(released && released->ok && !service.currentController().has_value() &&
                              countEvents(first, "controller.updated") == firstConflictBaseline + 1 &&
                              countEvents(second, "controller.updated") == secondConflictBaseline + 1,
                          "successful release commits once and remains correlated with the originating public request");

        result.expectTrue(firstConnection.receive(command("adapter-reacquire", frontend::ControllerAcquire{})).accepted(),
                          "the original frontend may reacquire controller ownership before its terminal-close probe");
        scheduler.drain();
        const std::size_t secondBeforeControllerClose = countEvents(second, "controller.updated");
        result.expectTrue(response(first, "adapter-reacquire") && response(first, "adapter-reacquire")->ok &&
                              core.snapshot().controller.has_value() && service.currentController() == firstConnection.sessionId(),
                          "BackendCore and ServerCore agree on the reacquired controller before frontend closure");
        firstConnection.close("adapter controller owner complete");
        scheduler.drain();
        result.expectTrue(!core.snapshot().controller.has_value() && !service.currentController().has_value() &&
                              countEvents(second, "controller.updated") == secondBeforeControllerClose + 1,
                          "closing the controller frontend releases BackendCore ownership and emits one canonical clear transition");

        Observations reentrant;
        frontend::FrontendConnection reentrantConnection;
        reentrantConnection = service.openConnection(remotePeer("127.0.0.1:41003"),
                                                     {[&reentrant, &reentrantConnection](const frontend::OutboundMessage& outbound) {
                                                          reentrant.messages.push_back(outbound.message);
                                                          if (std::holds_alternative<frontend::Welcome>(outbound.message)) {
                                                              reentrantConnection.close("reentrant callback close");
                                                          }
                                                          return true;
                                                      },
                                                      [&reentrant](const std::string& reason) {
                                                          reentrant.closes.push_back(reason);
                                                      }});
        expectedBackendSessionsBeforeAuthentication = 1;
        result.expectTrue(reentrantConnection.receive(frontend::ClientMessage{hello("adapter-token")}).accepted(),
                          "the reentrancy probe reaches the asynchronous public delivery border");
        scheduler.drain();
        result.expectTrue(!reentrantConnection.isOpen() && reentrant.closes.size() == 1 && service.isOpen() &&
                              service.authenticatedConnectionCount() == 1,
                          "a public send callback may reentrantly close its own connection without closing the service or remaining peer");

        secondConnection.close("adapter peer complete");
        scheduler.drain();
        result.expectTrue(core.snapshot().sessions.empty() && service.connectionCount() == 0 &&
                              authenticationObservedExpectedBackendSessions && authenticationCalls == 4 && successfulAuthentications == 3,
                          "public connection close releases its exact BackendCore session and leaves no half-open mapping");
        service.close("public adapter test complete");
        scheduler.drain();
        result.expectTrue(!service.isOpen(), "public service close delegates terminal shutdown exactly once");
    }

    void testBackendSessionAdmissionFailure(tests::support::TestResult& result) {
        const auto transport = std::make_shared<tests::codex::FakeTransportState>();
        ManualScheduler scheduler;
        backend::BackendCoreOptions backendOptions;
        backendOptions.capacity.maxSessions = 0;
        backendOptions.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        FakeBackendCore core(std::move(backendOptions), transport);

        frontend::FrontendServiceOptions options;
        options.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        options.timerScheduler = [](std::uint64_t, std::function<void()>) {
            return frontend::FrontendTimerCancellation{[] {
            }};
        };
        options.authenticator = [](const frontend::FrontendPeerContext&,
                                   const frontend::AuthenticationCredential&) -> frontend::AuthenticationResult {
            frontend::FrontendPrincipal principal;
            principal.id = "admission-principal";
            principal.profile = "adapter-test";
            principal.scopes = {frontend::FrontendScope::Observe};
            return frontend::AuthenticationSuccess{std::move(principal)};
        };
        frontend::FrontendService service(core, std::move(options));

        Observations observations;
        frontend::FrontendConnection connection = service.openConnection(remotePeer("127.0.0.1:41010"), callbacksFor(observations));
        const frontend::ConnectionReceiveResult received = connection.receive(frontend::ClientMessage{hello("admission-token")});
        result.expectTrue(received.status == frontend::ConnectionReceiveStatus::Closing && core.snapshot().sessions.empty() &&
                              service.authenticatedConnectionCount() == 0,
                          "BackendCore session admission failure leaves no half-open authenticated frontend session");
        scheduler.drain();

        std::size_t backendUnavailableErrors = 0;
        std::size_t welcomeMessages = 0;
        for (const frontend::ServerMessage& message : observations.messages) {
            if (const auto* error = std::get_if<frontend::ProtocolErrorMessage>(&message);
                error && error->code == frontend::ErrorCode::BackendUnavailable && error->closeConnection) {
                ++backendUnavailableErrors;
            }
            welcomeMessages += std::holds_alternative<frontend::Welcome>(message) ? 1U : 0U;
        }
        result.expectTrue(backendUnavailableErrors == 1 && welcomeMessages == 0 && observations.closes.size() == 1 &&
                              !connection.isOpen() && core.snapshot().sessions.empty(),
                          "post-authentication BackendCore admission failure drains one bounded backend_unavailable error before close");
    }

    void testCapacitySnapshotFeedbackQuiescence(tests::support::TestResult& result) {
        const auto transport = std::make_shared<tests::codex::FakeTransportState>();
        ManualScheduler scheduler;
        backend::BackendCoreOptions backendOptions;
        backendOptions.capacity.maxSnapshotBytes = 1;
        backendOptions.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        FakeBackendCore core(std::move(backendOptions), transport);

        frontend::FrontendServiceOptions options;
        options.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        frontend::FrontendService service(core, std::move(options));

        const backend::Snapshot first = core.snapshot();
        scheduler.drain(64);
        const backend::BackendState accounted = core.state();
        result.expectTrue(first.capacity.truncated && first.capacity.mandatoryCoreExceedsLimit &&
                              accounted.capacity.snapshotOmissions == 1 && scheduler.pending() == 0,
                          "one capacity-only BackendCore update is accounted and the frontend observer quiesces");

        const backend::SequenceNumber backendSequence = accounted.sequence;
        const frontend::SequenceNumber frontendSequence = service.currentSequence();
        const backend::Snapshot repeated = core.snapshot();
        scheduler.drain(64);
        const backend::BackendState stable = core.state();
        result.expectTrue(repeated.capacity.state.snapshotOmissions == 1 && stable.sequence == backendSequence &&
                              stable.capacity.snapshotOmissions == 1 && service.currentSequence() == frontendSequence &&
                              scheduler.pending() == 0,
                          "repeating the bounded Snapshot does not create a frontend/backend capacity feedback loop");

        service.close("capacity feedback probe complete");
        scheduler.drain(32);
    }

    void testReentrantServiceCloseFromCommandCompletion(tests::support::TestResult& result) {
        const auto transport = std::make_shared<tests::codex::FakeTransportState>();
        ManualScheduler scheduler;
        backend::BackendCoreOptions backendOptions;
        backendOptions.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        FakeBackendCore core(std::move(backendOptions), transport);

        frontend::FrontendServiceOptions options;
        options.scheduler = [](std::function<void()> callback) {
            callback();
        };
        options.authenticator = [](const frontend::FrontendPeerContext&,
                                   const frontend::AuthenticationCredential&) -> frontend::AuthenticationResult {
            frontend::FrontendPrincipal principal;
            principal.id = "close-principal";
            principal.profile = "adapter-test";
            principal.scopes = {frontend::FrontendScope::Observe, frontend::FrontendScope::Control};
            return frontend::AuthenticationSuccess{std::move(principal)};
        };
        frontend::FrontendService service(core, std::move(options));

        Observations observations;
        bool closeTriggered = false;
        frontend::FrontendConnection connection =
            service.openConnection(remotePeer("127.0.0.1:41011"),
                                   {[&](const frontend::OutboundMessage& outbound) {
                                        observations.messages.push_back(outbound.message);
                                        const auto* completed = std::get_if<frontend::Response>(&outbound.message);
                                        if (!closeTriggered && completed && completed->requestId == "close-acquire") {
                                            closeTriggered = true;
                                            service.close("reentrant command-completion service close");
                                        }
                                        return true;
                                    },
                                    [&observations](const std::string& reason) {
                                        observations.closes.push_back(reason);
                                    }});
        const frontend::ConnectionReceiveResult helloResult = connection.receive(frontend::ClientMessage{hello("close-token")});
        scheduler.drain();
        observations.messages.clear();
        const frontend::ConnectionReceiveResult commandResult = connection.receive(command("close-acquire", frontend::ControllerAcquire{}));
        const bool backendAcquiredBeforeCompletion = core.snapshot().controller.has_value();
        scheduler.drain();
        result.expectTrue(
            helloResult.accepted() && commandResult.accepted() && backendAcquiredBeforeCompletion && closeTriggered && !service.isOpen() &&
                !connection.isOpen() && observations.closes.size() == 1 && core.snapshot().sessions.empty() && !core.snapshot().controller,
            "a result callback may close the whole service inline while BackendCoreBridge retains its entered ServerCore target");
    }

    void testExternalBackendTopologyCompatibility(tests::support::TestResult& result) {
        const auto transport = std::make_shared<tests::codex::FakeTransportState>();
        ManualScheduler scheduler;
        backend::BackendCoreOptions backendOptions;
        backendOptions.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        backendOptions.maxEventsPerCallback = 128;
        FakeBackendCore core(std::move(backendOptions), transport);

        backend::FrontendSession preexistingExternal = core.openSession({});
        const std::string rawPreexistingId = std::to_string(preexistingExternal.id().value());
        scheduler.drain();

        frontend::FrontendServiceOptions options;
        options.journal = {1, 256U * 1024U, frontend::SequenceNumber{0}};
        options.batches = {16, 64U * 1024U};
        options.maxConnections = 4;
        options.maxUnauthenticatedConnections = 4;
        options.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        options.timerScheduler = [](std::uint64_t, std::function<void()>) {
            return frontend::FrontendTimerCancellation{[] {
            }};
        };
        options.authenticator = [](const frontend::FrontendPeerContext&,
                                   const frontend::AuthenticationCredential&) -> frontend::AuthenticationResult {
            frontend::FrontendPrincipal principal;
            principal.id = "external-topology-principal";
            principal.profile = "adapter-test";
            principal.scopes = {frontend::FrontendScope::Observe, frontend::FrontendScope::Control};
            return frontend::AuthenticationSuccess{std::move(principal)};
        };
        frontend::FrontendService service(core, std::move(options));

        Observations first;
        frontend::FrontendConnection firstConnection = service.openConnection(remotePeer("127.0.0.1:41020"), callbacksFor(first, &service));
        result.expectTrue(firstConnection.receive(frontend::ClientMessage{hello("external-topology-token")}).accepted(),
                          "the production public adapter accepts a frontend alongside a preexisting BackendCore consumer");
        scheduler.drain();
        const std::optional<std::string> firstIdentity = firstConnection.sessionId();
        const frontend::Snapshot* firstSnapshot = latestSnapshot(first);
        const std::vector<std::string> firstSnapshotSessions =
            firstSnapshot ? sessionIds(firstSnapshot->state) : std::vector<std::string>{};
        const std::optional<std::string> preexistingIdentity =
            identityOutside(firstSnapshotSessions, firstIdentity ? std::vector<std::string>{*firstIdentity} : std::vector<std::string>{});
        const std::optional<std::string> firstBridgeBackendId =
            identityOutside(backendSessionIds(core.snapshot()), std::vector<std::string>{rawPreexistingId});
        result.expectTrue(firstIdentity && firstSnapshot && preexistingIdentity && firstBridgeBackendId &&
                              exactIdentitySet(firstSnapshotSessions, {*firstIdentity, *preexistingIdentity}) &&
                              *preexistingIdentity != *firstIdentity && *preexistingIdentity != rawPreexistingId &&
                              std::find(firstSnapshotSessions.begin(), firstSnapshotSessions.end(), *firstBridgeBackendId) ==
                                  firstSnapshotSessions.end(),
                          "a preexisting external BackendCore session appears in the first production Snapshot under a private identity");

        const frontend::SequenceNumber beforeSecondHello = service.currentSequence();
        const std::size_t firstMessagesBeforeSecond = first.messages.size();
        Observations second;
        frontend::FrontendConnection secondConnection =
            service.openConnection(remotePeer("127.0.0.1:41021"), callbacksFor(second, &service));
        result.expectTrue(secondConnection.receive(frontend::ClientMessage{hello("external-topology-token")}).accepted(),
                          "a second bridge-owned frontend coexists with the external BackendCore consumer");
        scheduler.drain();
        const std::optional<std::string> secondIdentity = secondConnection.sessionId();
        const frontend::Snapshot* secondSnapshot = latestSnapshot(second);
        const std::vector<std::string> secondSnapshotSessions =
            secondSnapshot ? sessionIds(secondSnapshot->state) : std::vector<std::string>{};
        std::vector<std::string> knownBackendIds{rawPreexistingId};
        if (firstBridgeBackendId) {
            knownBackendIds.push_back(*firstBridgeBackendId);
        }
        const std::optional<std::string> secondBridgeBackendId = identityOutside(backendSessionIds(core.snapshot()), knownBackendIds);
        const std::vector<frontend::FrontendEvent> secondHelloEvents = eventsOfType(first, firstMessagesBeforeSecond, "sessions.updated");
        result.expectTrue(
            firstIdentity && secondIdentity && preexistingIdentity && secondSnapshot && secondBridgeBackendId &&
                service.currentSequence() == frontend::SequenceNumber{beforeSecondHello.value() + 1} &&
                exactIdentitySet(secondSnapshotSessions, {*firstIdentity, *secondIdentity, *preexistingIdentity}) &&
                secondHelloEvents.size() == 1 && secondHelloEvents.front().sequence == service.currentSequence() &&
                exactIdentitySet(sessionIds(secondHelloEvents.front().data), {*firstIdentity, *secondIdentity, *preexistingIdentity}) &&
                std::find(secondSnapshotSessions.begin(), secondSnapshotSessions.end(), *secondBridgeBackendId) ==
                    secondSnapshotSessions.end(),
            "two bridge-owned sessions and one external session appear exactly once without a BackendCore echo");

        const frontend::SequenceNumber beforePostConstructionOpen = service.currentSequence();
        const std::size_t firstMessagesBeforePostConstructionOpen = first.messages.size();
        const std::size_t secondMessagesBeforePostConstructionOpen = second.messages.size();
        backend::FrontendSession postConstructionExternal = core.openSession({});
        const std::string rawPostConstructionId = std::to_string(postConstructionExternal.id().value());
        scheduler.drain();
        const std::vector<frontend::FrontendEvent> firstPostConstructionEvents =
            eventsOfType(first, firstMessagesBeforePostConstructionOpen, "sessions.updated");
        const std::vector<frontend::FrontendEvent> secondPostConstructionEvents =
            eventsOfType(second, secondMessagesBeforePostConstructionOpen, "sessions.updated");
        const std::vector<std::string> postConstructionSessions =
            firstPostConstructionEvents.size() == 1 ? sessionIds(firstPostConstructionEvents.front().data) : std::vector<std::string>{};
        std::vector<std::string> knownIdentities;
        if (firstIdentity) {
            knownIdentities.push_back(*firstIdentity);
        }
        if (secondIdentity) {
            knownIdentities.push_back(*secondIdentity);
        }
        if (preexistingIdentity) {
            knownIdentities.push_back(*preexistingIdentity);
        }
        const std::optional<std::string> postConstructionIdentity = identityOutside(postConstructionSessions, knownIdentities);
        result.expectTrue(firstIdentity && secondIdentity && preexistingIdentity && postConstructionIdentity &&
                              service.currentSequence() == frontend::SequenceNumber{beforePostConstructionOpen.value() + 1} &&
                              firstPostConstructionEvents.size() == 1 && secondPostConstructionEvents.size() == 1 &&
                              firstPostConstructionEvents.front().sequence == service.currentSequence() &&
                              secondPostConstructionEvents.front().sequence == service.currentSequence() &&
                              exactIdentitySet(postConstructionSessions,
                                               {*firstIdentity, *secondIdentity, *preexistingIdentity, *postConstructionIdentity}) &&
                              exactIdentitySet(sessionIds(secondPostConstructionEvents.front().data),
                                               {*firstIdentity, *secondIdentity, *preexistingIdentity, *postConstructionIdentity}) &&
                              *postConstructionIdentity != rawPostConstructionId,
                          "an external BackendCore session opened after service construction advances and delivers one live transition");

        const std::size_t replayMessageBegin = first.messages.size();
        result.expectTrue(
            firstConnection.receive(command("external-topology-replay", frontend::ReplayAfter{beforePostConstructionOpen})).accepted(),
            "the production public adapter accepts replay from before an external-session transition");
        scheduler.drain();
        const frontend::Response* replayResponse = nullptr;
        const frontend::EventBatch* replayBatch = nullptr;
        const frontend::SyncComplete* replayComplete = nullptr;
        std::optional<std::size_t> replayResponseIndex;
        std::optional<std::size_t> replayBatchIndex;
        std::optional<std::size_t> replayCompleteIndex;
        std::size_t replayResponses = 0;
        std::size_t replayBatches = 0;
        std::size_t replayCompletes = 0;
        std::size_t replaySnapshots = 0;
        for (std::size_t index = replayMessageBegin; index < first.messages.size(); ++index) {
            const frontend::ServerMessage& message = first.messages[index];
            if (const auto* value = std::get_if<frontend::Response>(&message); value && value->requestId == "external-topology-replay") {
                replayResponse = value;
                replayResponseIndex = index;
                ++replayResponses;
            } else if (const auto* batch = std::get_if<frontend::EventBatch>(&message)) {
                replayBatch = batch;
                replayBatchIndex = index;
                ++replayBatches;
            } else if (const auto* complete = std::get_if<frontend::SyncComplete>(&message)) {
                replayComplete = complete;
                replayCompleteIndex = index;
                ++replayCompletes;
            } else if (std::holds_alternative<frontend::Snapshot>(message)) {
                ++replaySnapshots;
            }
        }
        const frontend::SequenceNumber postConstructionSequence{beforePostConstructionOpen.value() + 1};
        result.expectTrue(
            firstIdentity && secondIdentity && preexistingIdentity && postConstructionIdentity && replayResponse && replayResponse->ok &&
                replayResponse->result && replayResponse->result->value("syncMode", "") == "replay" &&
                replayResponse->result->value("sequence", std::uint64_t{0}) == postConstructionSequence.value() && replayResponses == 1 &&
                replayBatches == 1 && replayCompletes == 1 && replaySnapshots == 0 && replayBatch &&
                replayBatch->fromSequence == postConstructionSequence && replayBatch->toSequence == postConstructionSequence &&
                replayBatch->events.size() == 1 && replayBatch->events.front().sequence == postConstructionSequence &&
                replayBatch->events.front().type == "sessions.updated" &&
                exactIdentitySet(sessionIds(replayBatch->events.front().data),
                                 {*firstIdentity, *secondIdentity, *preexistingIdentity, *postConstructionIdentity}) &&
                replayComplete && replayComplete->sequence == postConstructionSequence && replayResponseIndex && replayBatchIndex &&
                replayCompleteIndex && *replayResponseIndex < *replayBatchIndex && *replayBatchIndex < *replayCompleteIndex,
            "the external-session transition replays at its exact sequence before one sync.complete");

        const std::size_t snapshotMessageBegin = second.messages.size();
        result.expectTrue(secondConnection.receive(command("external-topology-snapshot", frontend::SnapshotGet{})).accepted(),
                          "the production public adapter accepts an explicit topology Snapshot request");
        scheduler.drain();
        const frontend::Snapshot* composedSnapshot = latestSnapshot(second, snapshotMessageBegin);
        const std::vector<std::string> composedSessions =
            composedSnapshot ? sessionIds(composedSnapshot->state) : std::vector<std::string>{};
        result.expectTrue(
            firstIdentity && secondIdentity && preexistingIdentity && postConstructionIdentity && composedSnapshot &&
                exactIdentitySet(composedSessions, {*firstIdentity, *secondIdentity, *preexistingIdentity, *postConstructionIdentity}),
            "Snapshot barrier composition retains bridge-owned and external BackendCore sessions exactly once");

        const frontend::SequenceNumber beforeExternalAcquire = service.currentSequence();
        const std::size_t firstMessagesBeforeExternalAcquire = first.messages.size();
        const std::size_t secondMessagesBeforeExternalAcquire = second.messages.size();
        result.expectTrue(static_cast<bool>(preexistingExternal.submit("external-controller-acquire", backend::ControllerAcquire{})),
                          "the independent BackendCore consumer may acquire controller ownership");
        scheduler.drain();
        const std::vector<frontend::FrontendEvent> firstExternalAcquireEvents =
            eventsOfType(first, firstMessagesBeforeExternalAcquire, "controller.updated");
        const std::vector<frontend::FrontendEvent> secondExternalAcquireEvents =
            eventsOfType(second, secondMessagesBeforeExternalAcquire, "controller.updated");
        result.expectTrue(preexistingIdentity && core.snapshot().controller == preexistingExternal.id() &&
                              service.currentController() == preexistingIdentity &&
                              service.currentSequence() == frontend::SequenceNumber{beforeExternalAcquire.value() + 1} &&
                              firstExternalAcquireEvents.size() == 1 && secondExternalAcquireEvents.size() == 1 &&
                              firstExternalAcquireEvents.front().sequence == service.currentSequence() &&
                              secondExternalAcquireEvents.front().sequence == service.currentSequence() &&
                              eventControllerId(firstExternalAcquireEvents.front()) == preexistingIdentity &&
                              eventControllerId(secondExternalAcquireEvents.front()) == preexistingIdentity,
                          "external BackendCore controller ownership is published once under the same mapped identity");

        const frontend::SequenceNumber gapSequence = service.currentSequence();
        const std::size_t gapReplayBegin = second.messages.size();
        result.expectTrue(
            secondConnection.receive(command("external-controller-gap", frontend::ReplayAfter{beforePostConstructionOpen})).accepted(),
            "the production adapter accepts a replay cursor evicted by the external-controller transition");
        scheduler.drain();
        const frontend::Response* gapResponse = nullptr;
        const frontend::Snapshot* gapSnapshot = nullptr;
        const frontend::SyncComplete* gapComplete = nullptr;
        std::optional<std::size_t> gapResponseIndex;
        std::optional<std::size_t> gapSnapshotIndex;
        std::optional<std::size_t> gapCompleteIndex;
        std::size_t gapResponses = 0;
        std::size_t gapSnapshots = 0;
        std::size_t gapCompletes = 0;
        std::size_t gapBatches = 0;
        for (std::size_t index = gapReplayBegin; index < second.messages.size(); ++index) {
            const frontend::ServerMessage& message = second.messages[index];
            if (const auto* value = std::get_if<frontend::Response>(&message); value && value->requestId == "external-controller-gap") {
                gapResponse = value;
                gapResponseIndex = index;
                ++gapResponses;
            } else if (const auto* snapshot = std::get_if<frontend::Snapshot>(&message)) {
                gapSnapshot = snapshot;
                gapSnapshotIndex = index;
                ++gapSnapshots;
            } else if (const auto* complete = std::get_if<frontend::SyncComplete>(&message)) {
                gapComplete = complete;
                gapCompleteIndex = index;
                ++gapCompletes;
            } else if (std::holds_alternative<frontend::EventBatch>(message)) {
                ++gapBatches;
            }
        }
        const std::vector<std::string> gapSessions = gapSnapshot ? sessionIds(gapSnapshot->state) : std::vector<std::string>{};
        result.expectTrue(
            firstIdentity && secondIdentity && preexistingIdentity && postConstructionIdentity && gapResponse && gapResponse->ok &&
                gapResponse->result && gapResponse->result->value("syncMode", "") == "snapshot" &&
                gapResponse->result->value("sequence", std::uint64_t{0}) == gapSequence.value() && gapResponses == 1 && gapSnapshots == 1 &&
                gapCompletes == 1 && gapBatches == 0 && gapSnapshot && gapSnapshot->sequence == gapSequence &&
                exactIdentitySet(gapSessions, {*firstIdentity, *secondIdentity, *preexistingIdentity, *postConstructionIdentity}) &&
                controllerId(gapSnapshot->state) == preexistingIdentity && service.currentController() == preexistingIdentity &&
                gapComplete && gapComplete->sequence == gapSequence && gapResponseIndex && gapSnapshotIndex && gapCompleteIndex &&
                *gapResponseIndex < *gapSnapshotIndex && *gapSnapshotIndex < *gapCompleteIndex,
            "an evicted external topology replay falls back to one coherent Snapshot and sync.complete");

        const std::size_t firstMessagesBeforeConflict = first.messages.size();
        const std::size_t secondMessagesBeforeConflict = second.messages.size();
        result.expectTrue(firstConnection.receive(command("external-controller-conflict", frontend::ControllerAcquire{})).accepted() &&
                              preexistingIdentity && service.currentController() == preexistingIdentity,
                          "a frontend controller acquisition observes the external controller without a transient empty state");
        scheduler.drain();
        const frontend::Response* externalConflict = response(first, "external-controller-conflict");
        result.expectTrue(preexistingIdentity && externalConflict && !externalConflict->ok && externalConflict->error &&
                              externalConflict->error->code == frontend::ErrorCode::Conflict &&
                              service.currentController() == preexistingIdentity &&
                              eventsOfType(first, firstMessagesBeforeConflict, "controller.updated").empty() &&
                              eventsOfType(second, secondMessagesBeforeConflict, "controller.updated").empty(),
                          "frontend acquisition conflicts deterministically while external controller ownership remains unchanged");

        const frontend::SequenceNumber beforeExternalRelease = service.currentSequence();
        const std::size_t firstMessagesBeforeExternalRelease = first.messages.size();
        const std::size_t secondMessagesBeforeExternalRelease = second.messages.size();
        result.expectTrue(static_cast<bool>(preexistingExternal.submit("external-controller-release", backend::ControllerRelease{})),
                          "the external BackendCore controller may release ownership");
        scheduler.drain();
        const std::vector<frontend::FrontendEvent> firstExternalReleaseEvents =
            eventsOfType(first, firstMessagesBeforeExternalRelease, "controller.updated");
        const std::vector<frontend::FrontendEvent> secondExternalReleaseEvents =
            eventsOfType(second, secondMessagesBeforeExternalRelease, "controller.updated");
        result.expectTrue(!core.snapshot().controller && !service.currentController() &&
                              service.currentSequence() == frontend::SequenceNumber{beforeExternalRelease.value() + 1} &&
                              firstExternalReleaseEvents.size() == 1 && secondExternalReleaseEvents.size() == 1 &&
                              firstExternalReleaseEvents.front().sequence == service.currentSequence() &&
                              secondExternalReleaseEvents.front().sequence == service.currentSequence() &&
                              !eventControllerId(firstExternalReleaseEvents.front()) &&
                              !eventControllerId(secondExternalReleaseEvents.front()),
                          "external release clears the canonical controller exactly once");

        result.expectTrue(firstConnection.receive(command("frontend-acquire-after-external", frontend::ControllerAcquire{})).accepted(),
                          "a bridge-owned frontend can acquire after external ownership is released");
        scheduler.drain();
        const frontend::Response* acquiredAfterExternal = response(first, "frontend-acquire-after-external");
        result.expectTrue(acquiredAfterExternal && acquiredAfterExternal->ok && service.currentController() == firstIdentity,
                          "normal frontend controller acquisition resumes after external release");

        const std::size_t secondMessagesBeforeBridgeClose = second.messages.size();
        firstConnection.close("bridge-owned frontend close with external peers retained");
        scheduler.drain();
        const std::vector<frontend::FrontendEvent> bridgeCloseSessionEvents =
            eventsOfType(second, secondMessagesBeforeBridgeClose, "sessions.updated");
        const std::size_t postBridgeCloseSnapshotBegin = second.messages.size();
        result.expectTrue(secondConnection.receive(command("post-bridge-close-snapshot", frontend::SnapshotGet{})).accepted(),
                          "the remaining frontend can inspect topology after another bridge-owned frontend closes");
        scheduler.drain();
        const frontend::Snapshot* postBridgeCloseSnapshot = latestSnapshot(second, postBridgeCloseSnapshotBegin);
        const std::vector<std::string> postBridgeCloseSessions =
            postBridgeCloseSnapshot ? sessionIds(postBridgeCloseSnapshot->state) : std::vector<std::string>{};
        result.expectTrue(
            secondIdentity && preexistingIdentity && postConstructionIdentity && postBridgeCloseSnapshot &&
                exactIdentitySet(postBridgeCloseSessions, {*secondIdentity, *preexistingIdentity, *postConstructionIdentity}) &&
                bridgeCloseSessionEvents.size() == 1 &&
                exactIdentitySet(sessionIds(bridgeCloseSessionEvents.front().data),
                                 {*secondIdentity, *preexistingIdentity, *postConstructionIdentity}),
            "closing a bridge-owned frontend neither removes nor relabels either unrelated external session");

        const frontend::SequenceNumber beforeExternalClose = service.currentSequence();
        const std::size_t secondMessagesBeforeExternalClose = second.messages.size();
        postConstructionExternal.close("external consumer complete");
        scheduler.drain();
        const std::vector<frontend::FrontendEvent> externalCloseSessionEvents =
            eventsOfType(second, secondMessagesBeforeExternalClose, "sessions.updated");
        const std::size_t postExternalCloseSnapshotBegin = second.messages.size();
        result.expectTrue(secondConnection.receive(command("post-external-close-snapshot", frontend::SnapshotGet{})).accepted(),
                          "the remaining frontend can inspect topology after an external session closes");
        scheduler.drain();
        const frontend::Snapshot* postExternalCloseSnapshot = latestSnapshot(second, postExternalCloseSnapshotBegin);
        const std::vector<std::string> postExternalCloseSessions =
            postExternalCloseSnapshot ? sessionIds(postExternalCloseSnapshot->state) : std::vector<std::string>{};
        result.expectTrue(
            secondIdentity && preexistingIdentity && postExternalCloseSnapshot &&
                service.currentSequence() == frontend::SequenceNumber{beforeExternalClose.value() + 1} &&
                exactIdentitySet(postExternalCloseSessions, {*secondIdentity, *preexistingIdentity}) &&
                externalCloseSessionEvents.size() == 1 && externalCloseSessionEvents.front().sequence == service.currentSequence() &&
                exactIdentitySet(sessionIds(externalCloseSessionEvents.front().data), {*secondIdentity, *preexistingIdentity}),
            "external session close advances and publishes one removal without disturbing surviving topology");

        secondConnection.close("external topology adapter complete");
        preexistingExternal.close("preexisting external consumer complete");
        service.close("external topology compatibility test complete");
        scheduler.drain();
    }

    void testControllerCompletionAuthority(tests::support::TestResult& result) {
        const auto valid =
            [](generated::MethodId method, std::uint64_t expected, std::optional<std::uint64_t> controller, bool controllerRole) {
                return server::BackendCoreBridgeTestAccess::controllerResultValid(method, expected, std::move(controller), controllerRole);
            };
        result.expectTrue(valid(generated::MethodId::ControllerAcquire, 7, 7, true) &&
                              valid(generated::MethodId::ControllerRelease, 7, std::nullopt, false) &&
                              !valid(generated::MethodId::ControllerAcquire, 7, 8, true) &&
                              !valid(generated::MethodId::ControllerAcquire, 7, std::nullopt, true) &&
                              !valid(generated::MethodId::ControllerAcquire, 7, 7, false) &&
                              !valid(generated::MethodId::ControllerRelease, 7, 7, false) &&
                              !valid(generated::MethodId::ControllerRelease, 7, std::nullopt, true),
                          "controller completions require the exact BackendCore session and acquire/release role transition");
    }

    void testInlineBackendObserverAdmission(tests::support::TestResult& result) {
        const auto transport = std::make_shared<tests::codex::FakeTransportState>();
        backend::BackendCoreOptions backendOptions;
        backendOptions.scheduler = [](std::function<void()> callback) {
            callback();
        };
        FakeBackendCore core(std::move(backendOptions), transport);

        ManualScheduler scheduler;
        frontend::FrontendServiceOptions options;
        options.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        options.timerScheduler = [](std::uint64_t, std::function<void()>) {
            return frontend::FrontendTimerCancellation{[] {
            }};
        };
        options.authenticator = [](const frontend::FrontendPeerContext&,
                                   const frontend::AuthenticationCredential&) -> frontend::AuthenticationResult {
            frontend::FrontendPrincipal principal;
            principal.id = "inline-observer-principal";
            principal.profile = "adapter-test";
            principal.scopes = {frontend::FrontendScope::Observe, frontend::FrontendScope::Control};
            return frontend::AuthenticationSuccess{std::move(principal)};
        };
        frontend::FrontendService service(core, std::move(options));

        Observations first;
        Observations second;
        frontend::FrontendConnection firstConnection = service.openConnection(remotePeer("127.0.0.1:41030"), callbacksFor(first, &service));
        frontend::FrontendConnection secondConnection =
            service.openConnection(remotePeer("127.0.0.1:41031"), callbacksFor(second, &service));
        const bool firstAccepted = firstConnection.receive(frontend::ClientMessage{hello("inline-observer-token")}).accepted();
        scheduler.drain();
        const bool secondAccepted = secondConnection.receive(frontend::ClientMessage{hello("inline-observer-token")}).accepted();
        scheduler.drain();
        const std::optional<std::string> firstIdentity = firstConnection.sessionId();
        const std::optional<std::string> secondIdentity = secondConnection.sessionId();
        const frontend::Snapshot* secondSnapshot = latestSnapshot(second);
        const std::vector<std::string> initialSessions = secondSnapshot ? sessionIds(secondSnapshot->state) : std::vector<std::string>{};
        result.expectTrue(firstAccepted && secondAccepted && firstIdentity && secondIdentity && *firstIdentity != *secondIdentity &&
                              initialSessions.size() == 2 && containsIdentity(initialSessions, firstIdentity) &&
                              containsIdentity(initialSessions, secondIdentity),
                          "inline BackendCore observer delivery during admission cannot classify either bridge-owned session as external");

        const std::size_t sessionsBeforeBridgeClose = countEvents(second, "sessions.updated");
        firstConnection.close("inline bridge-owned close");
        scheduler.drain();
        backend::FrontendSession external = core.openSession({});
        scheduler.drain();
        const std::size_t snapshotBegin = second.messages.size();
        result.expectTrue(secondConnection.receive(command("inline-topology-snapshot", frontend::SnapshotGet{})).accepted(),
                          "the remaining frontend requests topology after an inline bridge close and external open");
        scheduler.drain();
        const frontend::Snapshot* snapshot = latestSnapshot(second, snapshotBegin);
        const std::vector<std::string> sessions = snapshot ? sessionIds(snapshot->state) : std::vector<std::string>{};
        const std::optional<std::string> externalIdentity =
            identityOutside(sessions, secondIdentity ? std::vector<std::string>{*secondIdentity} : std::vector<std::string>{});
        result.expectTrue(snapshot && secondIdentity && externalIdentity && sessions.size() == 2 &&
                              !containsIdentity(sessions, firstIdentity) && containsIdentity(sessions, secondIdentity) &&
                              countEvents(second, "sessions.updated") == sessionsBeforeBridgeClose + 2,
                          "retired inline bridge topology remains suppressed while the next independent BackendCore session maps once");

        const std::size_t sessionsBeforeExternalClose = countEvents(second, "sessions.updated");
        external.close("inline external close");
        scheduler.drain();
        result.expectTrue(countEvents(second, "sessions.updated") == sessionsBeforeExternalClose + 1 && first.outboundConversionValid &&
                              second.outboundConversionValid,
                          "inline external close publishes once and all production outbound adapter conversions remain valid");

        secondConnection.close("inline observer admission complete");
        service.close("inline observer admission complete");
        scheduler.drain();
    }

    void testInlineObserverResynchronizationAdmission(tests::support::TestResult& result) {
        const auto transport = std::make_shared<tests::codex::FakeTransportState>();
        backend::BackendCoreOptions backendOptions;
        backendOptions.maxObserverQueueEntries = 0;
        backendOptions.scheduler = [](std::function<void()> callback) {
            callback();
        };
        FakeBackendCore core(std::move(backendOptions), transport);

        ManualScheduler scheduler;
        frontend::FrontendServiceOptions options;
        options.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        options.timerScheduler = [](std::uint64_t, std::function<void()>) {
            return frontend::FrontendTimerCancellation{[] {
            }};
        };
        options.authenticator = [](const frontend::FrontendPeerContext&,
                                   const frontend::AuthenticationCredential&) -> frontend::AuthenticationResult {
            frontend::FrontendPrincipal principal;
            principal.id = "inline-resynchronization-principal";
            principal.profile = "adapter-test";
            principal.scopes = {frontend::FrontendScope::Observe};
            return frontend::AuthenticationSuccess{std::move(principal)};
        };
        frontend::FrontendService service(core, std::move(options));

        Observations observations;
        frontend::FrontendConnection connection =
            service.openConnection(remotePeer("127.0.0.1:41035"), callbacksFor(observations, &service));
        const bool accepted = connection.receive(frontend::ClientMessage{hello("inline-resynchronization-token")}).accepted();
        scheduler.drain();
        const std::optional<std::string> identity = connection.sessionId();
        const frontend::Snapshot* snapshot = latestSnapshot(observations);
        const std::vector<std::string> visibleSessions = snapshot ? sessionIds(snapshot->state) : std::vector<std::string>{};
        result.expectTrue(accepted && identity && core.snapshot().sessions.size() == 1 && snapshot &&
                              exactIdentitySet(visibleSessions, {*identity}) && observations.outboundConversionValid,
                          "an inline BackendCore observer resynchronization during admission cannot expose the private bridge session ID");

        connection.close("inline resynchronization admission complete");
        service.close("inline resynchronization admission complete");
        scheduler.drain();
    }

    void testControllerCompletionWaitsForObserverFence(tests::support::TestResult& result) {
        const auto transport = std::make_shared<tests::codex::FakeTransportState>();
        ManualScheduler scheduler;
        backend::BackendCoreOptions backendOptions;
        backendOptions.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        backendOptions.maxEventsPerCallback = 1;
        FakeBackendCore core(std::move(backendOptions), transport);

        frontend::FrontendServiceOptions options;
        options.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        options.timerScheduler = [](std::uint64_t, std::function<void()>) {
            return frontend::FrontendTimerCancellation{[] {
            }};
        };
        options.authenticator = [](const frontend::FrontendPeerContext&,
                                   const frontend::AuthenticationCredential&) -> frontend::AuthenticationResult {
            frontend::FrontendPrincipal principal;
            principal.id = "observer-fence-principal";
            principal.profile = "adapter-test";
            principal.scopes = {frontend::FrontendScope::Observe, frontend::FrontendScope::Control};
            return frontend::AuthenticationSuccess{std::move(principal)};
        };
        frontend::FrontendService service(core, std::move(options));

        Observations observations;
        frontend::FrontendConnection connection =
            service.openConnection(remotePeer("127.0.0.1:41040"), callbacksFor(observations, &service));
        result.expectTrue(connection.receive(frontend::ClientMessage{hello("observer-fence-token")}).accepted(),
                          "the observer-fence probe authenticates one bridge-owned frontend");
        scheduler.drain();
        const std::optional<std::string> frontendIdentity = connection.sessionId();
        const frontend::SequenceNumber baseline = service.currentSequence();
        const std::size_t messageBegin = observations.messages.size();

        backend::FrontendSession externalA = core.openSession({});
        backend::FrontendSession externalB = core.openSession({});
        const std::string rawExternalA = std::to_string(externalA.id().value());
        const std::string rawExternalB = std::to_string(externalB.id().value());
        const bool commandAccepted = connection.receive(command("observer-fence-acquire", frontend::ControllerAcquire{})).accepted();
        const std::size_t callbacksBeforeFirstBatch = scheduler.pending();
        const bool firstObserverBatchRan = scheduler.runOne();
        const std::size_t callbacksAfterFirstBatch = scheduler.pending();
        const bool completionRanBeforeRemainingObserverBatches = scheduler.runOne();
        result.expectTrue(commandAccepted && firstObserverBatchRan && completionRanBeforeRemainingObserverBatches &&
                              callbacksBeforeFirstBatch == 2 && callbacksAfterFirstBatch == 3 && scheduler.pending() == 2 &&
                              core.snapshot().controller.has_value() && !service.currentController() &&
                              response(observations, "observer-fence-acquire") == nullptr && service.currentSequence() == baseline,
                          "a controller completion racing queued one-event observer batches remains fenced from publication");

        scheduler.drain();
        const frontend::Response* acquired = response(observations, "observer-fence-acquire");
        const std::vector<frontend::FrontendEvent> sessionEvents = eventsOfType(observations, messageBegin, "sessions.updated");
        const std::vector<frontend::FrontendEvent> controllerEvents = eventsOfType(observations, messageBegin, "controller.updated");
        const std::vector<std::string> finalEventSessions =
            sessionEvents.empty() ? std::vector<std::string>{} : sessionIds(sessionEvents.back().data);
        std::vector<std::string> mappedExternalIdentities = finalEventSessions;
        if (frontendIdentity) {
            std::erase(mappedExternalIdentities, *frontendIdentity);
        }
        result.expectTrue(
            frontendIdentity && acquired && acquired->ok && service.currentController() == frontendIdentity &&
                service.currentSequence() == frontend::SequenceNumber{baseline.value() + 3} && sessionEvents.size() == 2 &&
                sessionEvents[0].sequence == frontend::SequenceNumber{baseline.value() + 1} &&
                sessionEvents[1].sequence == frontend::SequenceNumber{baseline.value() + 2} && controllerEvents.size() == 1 &&
                controllerEvents.front().sequence == service.currentSequence() &&
                eventControllerId(controllerEvents.front()) == frontendIdentity && mappedExternalIdentities.size() == 2 &&
                exactIdentitySet(finalEventSessions, {*frontendIdentity, mappedExternalIdentities[0], mappedExternalIdentities[1]}) &&
                mappedExternalIdentities[0] != rawExternalA && mappedExternalIdentities[0] != rawExternalB &&
                mappedExternalIdentities[1] != rawExternalA && mappedExternalIdentities[1] != rawExternalB,
            "the observer fence publishes two external sessions followed by one non-transient frontend controller fact");

        const std::size_t snapshotBegin = observations.messages.size();
        result.expectTrue(connection.receive(command("observer-fence-snapshot", frontend::SnapshotGet{})).accepted(),
                          "the observer-fence probe requests its composed final topology");
        scheduler.drain();
        const frontend::Snapshot* snapshot = latestSnapshot(observations, snapshotBegin);
        result.expectTrue(snapshot && frontendIdentity && mappedExternalIdentities.size() == 2 &&
                              exactIdentitySet(sessionIds(snapshot->state),
                                               {*frontendIdentity, mappedExternalIdentities[0], mappedExternalIdentities[1]}) &&
                              controllerId(snapshot->state) == frontendIdentity,
                          "the post-race Snapshot agrees with the journaled session and controller topology");

        connection.close("observer-fence probe complete");
        externalA.close("observer-fence external A complete");
        externalB.close("observer-fence external B complete");
        service.close("observer-fence probe complete");
        scheduler.drain();
    }

    void testUnexpectedBackendClosePreservesExternalControllerHandoff(tests::support::TestResult& result) {
        const auto transport = std::make_shared<tests::codex::FakeTransportState>();
        ManualScheduler scheduler;
        backend::BackendCoreOptions backendOptions;
        backendOptions.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        backendOptions.maxEventsPerCallback = 1;
        FakeBackendCore core(std::move(backendOptions), transport);

        backend::FrontendSession external = core.openSession({});
        scheduler.drain();

        frontend::FrontendServiceOptions options;
        options.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        options.timerScheduler = [](std::uint64_t, std::function<void()>) {
            return frontend::FrontendTimerCancellation{[] {
            }};
        };
        options.authenticator = [](const frontend::FrontendPeerContext&,
                                   const frontend::AuthenticationCredential&) -> frontend::AuthenticationResult {
            frontend::FrontendPrincipal principal;
            principal.id = "unexpected-close-principal";
            principal.profile = "adapter-test";
            principal.scopes = {frontend::FrontendScope::Observe, frontend::FrontendScope::Control};
            return frontend::AuthenticationSuccess{std::move(principal)};
        };
        frontend::FrontendService service(core, std::move(options));

        Observations first;
        Observations second;
        frontend::FrontendConnection firstConnection = service.openConnection(remotePeer("127.0.0.1:41050"), callbacksFor(first, &service));
        frontend::FrontendConnection secondConnection =
            service.openConnection(remotePeer("127.0.0.1:41051"), callbacksFor(second, &service));
        const bool firstHello = firstConnection.receive(frontend::ClientMessage{hello("unexpected-close-token")}).accepted();
        scheduler.drain();
        const bool secondHello = secondConnection.receive(frontend::ClientMessage{hello("unexpected-close-token")}).accepted();
        scheduler.drain();
        const std::optional<std::string> firstIdentity = firstConnection.sessionId();
        const std::optional<std::string> secondIdentity = secondConnection.sessionId();
        const frontend::Snapshot* initialSnapshot = latestSnapshot(second);
        const std::vector<std::string> initialSessions = initialSnapshot ? sessionIds(initialSnapshot->state) : std::vector<std::string>{};
        std::vector<std::string> bridgeIdentities;
        if (firstIdentity) {
            bridgeIdentities.push_back(*firstIdentity);
        }
        if (secondIdentity) {
            bridgeIdentities.push_back(*secondIdentity);
        }
        const std::optional<std::string> externalIdentity = identityOutside(initialSessions, bridgeIdentities);
        result.expectTrue(firstHello && secondHello && firstIdentity && secondIdentity && externalIdentity &&
                              exactIdentitySet(initialSessions, {*firstIdentity, *secondIdentity, *externalIdentity}),
                          "the unexpected-close probe starts Ready with one external and two bridge-owned sessions");

        result.expectTrue(firstConnection.receive(command("unexpected-close-acquire", frontend::ControllerAcquire{})).accepted(),
                          "frontend A acquires controller before its BackendCore command-session overflow");
        scheduler.drain();
        result.expectTrue(response(first, "unexpected-close-acquire") && response(first, "unexpected-close-acquire")->ok &&
                              service.currentController() == firstIdentity,
                          "frontend A controller acquisition is fully committed before the unexpected-close race");

        // Keep one external topology event ahead of A's release command. With
        // one event per observer callback, the command completion must wait for
        // the shared observer fence rather than committing a transient clear.
        backend::FrontendSession backlogExternal = core.openSession({});
        const bool releaseAccepted = firstConnection.receive(command("unexpected-close-release", frontend::ControllerRelease{})).accepted();
        const bool backlogObserverRan = scheduler.runOne();
        service.flush();
        const std::size_t secondMessageBegin = second.messages.size();
        const bool releaseCommandRan = scheduler.runOne();
        result.expectTrue(releaseAccepted && backlogObserverRan && releaseCommandRan && !core.snapshot().controller &&
                              service.currentController() == firstIdentity && response(first, "unexpected-close-release") == nullptr,
                          "A's release completion remains fenced after BackendCore has committed it");

        const bool backendAClosedBeforeHandoff =
            frontend::FrontendServiceTestAccess::closeBackendSession(service, firstConnection, "forced backend-side command-session close");
        const bool externalAcquireAccepted =
            static_cast<bool>(external.submit("unexpected-close-external-acquire", backend::ControllerAcquire{}));
        result.expectTrue(backendAClosedBeforeHandoff && externalAcquireAccepted && core.snapshot().controller == external.id() &&
                              firstConnection.isOpen() && service.currentController() == firstIdentity &&
                              response(first, "unexpected-close-release") == nullptr,
                          "BackendCore closes A and hands controller to the external session before queued observer callbacks run");
        scheduler.drain();

        const std::vector<frontend::FrontendEvent> controllerEvents = eventsOfType(second, secondMessageBegin, "controller.updated");
        const std::vector<frontend::FrontendEvent> sessionEvents = eventsOfType(second, secondMessageBegin, "sessions.updated");
        const std::vector<std::string> survivingSessions =
            sessionEvents.size() == 1 ? sessionIds(sessionEvents.front().data) : std::vector<std::string>{};
        std::vector<std::string> externalIdentities = survivingSessions;
        if (secondIdentity) {
            std::erase(externalIdentities, *secondIdentity);
        }
        const std::vector<std::string> knownExternalIdentity =
            externalIdentity ? std::vector<std::string>{*externalIdentity} : std::vector<std::string>{};
        const std::optional<std::string> backlogExternalIdentity = identityOutside(externalIdentities, knownExternalIdentity);
        const bool externalRetained =
            std::any_of(core.snapshot().sessions.begin(), core.snapshot().sessions.end(), [&](const backend::SessionSnapshot& session) {
                return session.id == external.id();
            });
        result.expectTrue(firstIdentity && secondIdentity && externalIdentity && backlogExternalIdentity,
                          "the unexpected-close handoff retains all mapped identities");
        result.expectTrue(!firstConnection.isOpen() && secondConnection.isOpen() && first.closes.size() == 1 && externalRetained,
                          "the unexpected backend close terminates only A");
        result.expectTrue(response(first, "unexpected-close-release") == nullptr,
                          "A's late release completion is invalidated by its unexpected backend close");
        result.expectTrue(externalIdentity && service.currentController() == externalIdentity,
                          "currentController reports the external handoff owner");
        result.expectEqual(
            std::size_t{1}, controllerEvents.size(), "B observes exactly one controller event for the direct A-to-external handoff");
        result.expectTrue(externalIdentity && !controllerEvents.empty() && eventControllerId(controllerEvents.front()) == externalIdentity,
                          "the direct handoff event names the mapped external owner without a transient clear");
        result.expectTrue(firstIdentity && secondIdentity && externalIdentity && backlogExternalIdentity && sessionEvents.size() == 1 &&
                              exactIdentitySet(survivingSessions, {*secondIdentity, *externalIdentity, *backlogExternalIdentity}) &&
                              !containsIdentity(survivingSessions, firstIdentity),
                          "B observes one A removal while both unrelated external sessions retain their identities");
        result.expectTrue(backlogExternalIdentity && *backlogExternalIdentity != std::to_string(backlogExternal.id().value()),
                          "the backlog external session's private BackendCore integer is not exposed as its frontend identity");

        const std::size_t snapshotBegin = second.messages.size();
        result.expectTrue(secondConnection.receive(command("unexpected-close-snapshot", frontend::SnapshotGet{})).accepted(),
                          "frontend B requests the authoritative topology after the unexpected close");
        scheduler.drain();
        const frontend::Snapshot* snapshot = latestSnapshot(second, snapshotBegin);
        result.expectTrue(
            snapshot && secondIdentity && externalIdentity && backlogExternalIdentity &&
                exactIdentitySet(sessionIds(snapshot->state), {*secondIdentity, *externalIdentity, *backlogExternalIdentity}) &&
                controllerId(snapshot->state) == externalIdentity && service.currentController() == externalIdentity &&
                core.snapshot().controller == external.id(),
            "Snapshot, currentController, and BackendCore agree on the retained external controller");

        secondConnection.close("unexpected-close observer complete");
        external.close("unexpected-close external complete");
        backlogExternal.close("unexpected-close backlog external complete");
        service.close("unexpected-close probe complete");
        scheduler.drain();
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testPublicAdapter(result);
    testBackendSessionAdmissionFailure(result);
    testCapacitySnapshotFeedbackQuiescence(result);
    testReentrantServiceCloseFromCommandCompletion(result);
    testExternalBackendTopologyCompatibility(result);
    testControllerCompletionAuthority(result);
    testInlineBackendObserverAdmission(result);
    testInlineObserverResynchronizationAdmission(result);
    testControllerCompletionWaitsForObserverFence(result);
    testUnexpectedBackendClosePreservesExternalControllerHandoff(result);
    return result.processResult();
}
