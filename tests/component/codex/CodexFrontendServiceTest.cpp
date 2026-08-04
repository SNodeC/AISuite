/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CodexBackendTestSupport.h"
#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/backend/Snapshot.h"
#include "ai/openai/codex/frontend/EventCoalescer.h"
#include "ai/openai/codex/frontend/FrontendService.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace backend = ai::openai::codex::backend;
    namespace frontend = ai::openai::codex::frontend;

    using FakeBackendCore = backend::BackendCore<tests::codex::FakeAppServerClient>;

    using ai::openai::codex::Error;
    using ai::openai::codex::Json;
    using ai::openai::codex::detail::TransportCallbacks;

    Json agentItemValue(const std::string& id, const std::string& text = {}) {
        return {{"type", "agentMessage"}, {"id", id}, {"text", text}};
    }

    struct LegacyMetadataItemFixture {
        std::string id;
        std::string type;
        Json item;
    };

    std::vector<LegacyMetadataItemFixture> legacyMetadataItemFixtures() {
        return {
            {"v1-collab-agent",
             "collabAgentToolCall",
             {{"type", "collabAgentToolCall"},
              {"id", "v1-collab-agent"},
              {"agentsStates", {{"worker", {{"message", "private worker message"}, {"status", "pendingInit"}}}}},
              {"model", "private-model"},
              {"prompt", "private prompt"},
              {"reasoningEffort", "high"},
              {"receiverThreadIds", Json::array({"private-receiver"})},
              {"senderThreadId", "private-sender"},
              {"status", "inProgress"},
              {"tool", "spawnAgent"}}},
            {"v1-context-compaction", "contextCompaction", {{"type", "contextCompaction"}, {"id", "v1-context-compaction"}}},
            {"v1-entered-review",
             "enteredReviewMode",
             {{"type", "enteredReviewMode"}, {"id", "v1-entered-review"}, {"review", "private review"}}},
            {"v1-exited-review",
             "exitedReviewMode",
             {{"type", "exitedReviewMode"}, {"id", "v1-exited-review"}, {"review", "private review"}}},
            {"v1-hook-prompt",
             "hookPrompt",
             {{"type", "hookPrompt"},
              {"id", "v1-hook-prompt"},
              {"fragments", Json::array({Json{{"hookRunId", "private-hook"}, {"text", "private hook prompt"}}})}}},
            {"v1-image-generation",
             "imageGeneration",
             {{"type", "imageGeneration"},
              {"id", "v1-image-generation"},
              {"result", "private image result"},
              {"revisedPrompt", "private revised prompt"},
              {"savedPath", "/private/image.png"},
              {"status", "completed"}}},
            {"v1-image-view", "imageView", {{"type", "imageView"}, {"id", "v1-image-view"}, {"path", "/private/image.png"}}},
            {"v1-plan", "plan", {{"type", "plan"}, {"id", "v1-plan"}, {"text", "private plan"}}},
            {"v1-sleep", "sleep", {{"type", "sleep"}, {"id", "v1-sleep"}, {"durationMs", 25}}},
            {"v1-sub-agent",
             "subAgentActivity",
             {{"type", "subAgentActivity"},
              {"id", "v1-sub-agent"},
              {"agentPath", "private/worker"},
              {"agentThreadId", "private-worker-thread"},
              {"kind", "started"}}},
        };
    }

    class ManualScheduler {
    public:
        void schedule(std::function<void()> callback) {
            callbacks.push_back(std::move(callback));
            ++scheduled;
        }

        bool runOne() {
            if (callbacks.empty()) {
                return false;
            }
            std::function<void()> callback = std::move(callbacks.front());
            callbacks.pop_front();
            callback();
            ++executed;
            return true;
        }

        void drain(std::size_t limit = 100'000) {
            std::size_t count = 0;
            while (runOne()) {
                if (++count >= limit) {
                    throw std::runtime_error("manual scheduler drain limit exceeded");
                }
            }
        }

        std::size_t pending() const noexcept {
            return callbacks.size();
        }

        std::size_t scheduled = 0;
        std::size_t executed = 0;

    private:
        std::deque<std::function<void()>> callbacks;
    };

    class ManualTimerScheduler {
    public:
        struct Entry {
            std::uint64_t delayMs = 0;
            std::function<void()> callback;
            std::shared_ptr<bool> active;
        };

        frontend::FrontendTimerCancellation schedule(std::uint64_t delayMs, std::function<void()> callback) {
            auto active = std::make_shared<bool>(true);
            entries.push_back({delayMs, std::move(callback), active});
            return [active] {
                *active = false;
            };
        }

        bool fire(std::size_t index) {
            if (index >= entries.size() || !*entries[index].active) {
                return false;
            }
            *entries[index].active = false;
            entries[index].callback();
            return true;
        }

        std::vector<Entry> entries;
    };

    struct Observations {
        std::vector<frontend::ServerMessage> messages;
        std::vector<std::string> compactJson;
        std::vector<std::size_t> serializedBytes;
        std::vector<std::string> closeReasons;
    };

    frontend::FrontendConnectionCallbacks callbacksFor(Observations& observations) {
        return {[&observations](const frontend::OutboundMessage& message) {
                    observations.messages.push_back(message.message);
                    observations.compactJson.push_back(message.compactJson);
                    observations.serializedBytes.push_back(message.serializedBytes);
                    return true;
                },
                [&observations](const std::string& reason) {
                    observations.closeReasons.push_back(reason);
                }};
    }

    frontend::ClientMessage hello(std::optional<frontend::SequenceNumber> resumeAfter = std::nullopt) {
        return frontend::Hello{resumeAfter, frontend::Json::object()};
    }

    constexpr std::uint64_t TrustedTestUserId = 4242;
    constexpr std::string_view SparseSequenceRemoteToken = "sparse-sequence-bearer";

    frontend::FrontendPeerContext trustedPeer() {
        frontend::FrontendPeerContext peer;
        peer.transport = frontend::FrontendTransportKind::Unix;
        peer.loopback = true;
        peer.localPeer = true;
        peer.unixUserId = TrustedTestUserId;
        return peer;
    }

    void enableVerifiedTestTrust(frontend::FrontendServiceOptions& options) {
        options.trustedLocalUserId = TrustedTestUserId;
        options.timerScheduler = [](std::uint64_t, std::function<void()>) {
            return frontend::FrontendTimerCancellation{[] {
            }};
        };
    }

    frontend::ClientMessage command(std::string requestId, frontend::CommandParameters parameters) {
        return frontend::Command{std::move(requestId), std::move(parameters), frontend::Json::object(), frontend::Json::object()};
    }

    const frontend::Welcome* welcome(const Observations& observations) {
        for (const frontend::ServerMessage& message : observations.messages) {
            if (const auto* value = std::get_if<frontend::Welcome>(&message)) {
                return value;
            }
        }
        return nullptr;
    }

    const frontend::Response* response(const Observations& observations, const std::string& requestId) {
        for (const frontend::ServerMessage& message : observations.messages) {
            if (const auto* value = std::get_if<frontend::Response>(&message); value && value->requestId == requestId) {
                return value;
            }
        }
        return nullptr;
    }

    bool responseHasError(const Observations& observations, const std::string& requestId, frontend::ErrorCode code) {
        const frontend::Response* value = response(observations, requestId);
        return value && !value->ok && value->error && value->error->code == code;
    }

    const frontend::ProtocolErrorMessage* protocolError(const Observations& observations, const std::optional<std::string>& requestId) {
        for (const frontend::ServerMessage& message : observations.messages) {
            if (const auto* value = std::get_if<frontend::ProtocolErrorMessage>(&message); value && value->requestId == requestId) {
                return value;
            }
        }
        return nullptr;
    }

    bool hasSuccessfulResponse(const Observations& observations, const std::string& requestId) {
        for (const frontend::ServerMessage& message : observations.messages) {
            if (const auto* value = std::get_if<frontend::Response>(&message); value && value->requestId == requestId && value->ok) {
                return true;
            }
        }
        return false;
    }

    std::size_t countSnapshots(const Observations& observations) {
        std::size_t result = 0;
        for (const frontend::ServerMessage& message : observations.messages) {
            result += std::holds_alternative<frontend::Snapshot>(message) ? 1U : 0U;
        }
        return result;
    }

    const frontend::Snapshot* latestSnapshot(const Observations& observations) {
        for (auto iterator = observations.messages.rbegin(); iterator != observations.messages.rend(); ++iterator) {
            if (const auto* snapshot = std::get_if<frontend::Snapshot>(&*iterator)) {
                return snapshot;
            }
        }
        return nullptr;
    }

    std::vector<frontend::FrontendEvent> events(const Observations& observations) {
        std::vector<frontend::FrontendEvent> result;
        for (const frontend::ServerMessage& message : observations.messages) {
            if (const auto* batch = std::get_if<frontend::EventBatch>(&message)) {
                result.insert(result.end(), batch->events.begin(), batch->events.end());
            }
        }
        return result;
    }

    void testCoalescingAndBounds(tests::support::TestResult& result) {
        frontend::EventCoalescer coalescer({32});
        const frontend::CoalescingKey agentKey = frontend::CoalescingKey::itemContent("thread-1", "turn-1", "item-1", "agentText");

        std::string accumulated;
        std::size_t schedulingRequests = 0;
        for (std::size_t index = 0; index < 1000; ++index) {
            accumulated += static_cast<char>('a' + static_cast<int>(index % 26));
            const frontend::CoalescerMarkResult marked =
                coalescer.mark({agentKey,
                                "item.content.updated",
                                frontend::Json{{"itemId", "item-1"}, {"channel", "agentText"}, {"content", accumulated}},
                                frontend::FlushUrgency::Deferred});
            schedulingRequests += marked.scheduleRequired ? 1U : 0U;
        }
        result.expectTrue(coalescer.dirtyCount() == 1 && schedulingRequests == 1 && coalescer.flushScheduled(),
                          "1,000 raw deltas dirty one entity and request exactly one next-tick flush");

        const frontend::CoalescerDrainResult drained = coalescer.drain();
        result.expectTrue(drained.updates.size() == 1 && drained.updates.front().data["content"] == accumulated,
                          "coalescing preserves the exact final accumulated agent text");

        const frontend::FrontendEvent coalescedEvent{
            frontend::SequenceNumber{1}, drained.updates.front().type, drained.updates.front().data, frontend::Json::object()};
        frontend::UpdateBatchBuilder batches({16, 16U * 1024U});
        const auto built = batches.build({coalescedEvent});
        result.expectTrue(built.success() && built.batches.size() == 1 && built.batches.front().batch.events.size() == 1 &&
                              built.batches.front().batch.events.size() < 1000,
                          "1,000 token deltas become one normalized frontend message, substantially below raw granularity");

        const auto reasoning = coalescer.mark({frontend::CoalescingKey::itemContent("thread-1", "turn-1", "item-1", "reasoningText"),
                                               "item.content.updated",
                                               frontend::Json{{"content", "reasoning-final"}},
                                               frontend::FlushUrgency::Deferred});
        const auto commandOutput = coalescer.mark({frontend::CoalescingKey::itemContent("thread-1", "turn-1", "item-1", "commandOutput"),
                                                   "item.content.updated",
                                                   frontend::Json{{"content", "command-final"}},
                                                   frontend::FlushUrgency::Deferred});
        const auto otherTurn = coalescer.mark({frontend::CoalescingKey::itemContent("thread-1", "turn-2", "item-1", "reasoningText"),
                                               "item.content.updated",
                                               frontend::Json{{"content", "other-turn"}},
                                               frontend::FlushUrgency::Deferred});
        result.expectTrue(reasoning.accepted() && commandOutput.accepted() && otherTurn.accepted() && coalescer.dirtyCount() == 3,
                          "reasoning, command output, and a same-named item in another turn never coalesce together");

        const frontend::CoalescerMarkResult terminal = coalescer.mark({frontend::CoalescingKey::item("thread-1", "turn-1", "item-1"),
                                                                       "item.updated",
                                                                       frontend::Json{{"status", "completed"}},
                                                                       frontend::FlushUrgency::Immediate});
        result.expectTrue(terminal.immediateFlush, "item completion upgrades a pending flush to immediate");
        const frontend::CoalescerDrainResult terminalDrain = coalescer.drain();
        result.expectTrue(terminalDrain.updates.size() == 4 && terminalDrain.updates.back().type == "item.updated",
                          "terminal flush preserves independent dirty-entity insertion order");

        frontend::EventCoalescer terminalOrdering({8});
        const frontend::CoalescingKey turnKey = frontend::CoalescingKey::turn("thread-order", "turn-order");
        result.expectTrue(terminalOrdering
                              .mark({turnKey,
                                     "turn.updated",
                                     frontend::Json{{"turn", {{"id", "turn-order"}, {"terminal", false}}}},
                                     frontend::FlushUrgency::Deferred})
                              .accepted(),
                          "turn start dirties the turn key before content arrives");
        result.expectTrue(terminalOrdering
                              .mark({frontend::CoalescingKey::itemContent("thread-order", "turn-order", "item-order", "agentText"),
                                     "item.content.updated",
                                     frontend::Json{{"itemId", "item-order"}, {"channel", "agentText"}, {"content", "final"}},
                                     frontend::FlushUrgency::Deferred})
                              .accepted(),
                          "final item content remains independently dirty from its turn");
        result.expectTrue(terminalOrdering
                              .mark({turnKey,
                                     "turn.updated",
                                     frontend::Json{{"turn", {{"id", "turn-order"}, {"terminal", true}}}},
                                     frontend::FlushUrgency::Immediate})
                              .accepted(),
                          "turn completion replaces its earlier dirty turn state");
        const frontend::CoalescerDrainResult orderedTerminalDrain = terminalOrdering.drain();
        result.expectTrue(orderedTerminalDrain.updates.size() == 2 && orderedTerminalDrain.updates[0].type == "item.content.updated" &&
                              orderedTerminalDrain.updates[1].type == "turn.updated" &&
                              orderedTerminalDrain.updates[1].data["turn"]["terminal"] == true,
                          "final item content precedes terminal turn state even when turn.started dirtied the key first");

        frontend::EventCoalescer bounded({1});
        result.expectTrue(
            bounded
                .mark(
                    {frontend::CoalescingKey::thread("one"), "thread.updated", frontend::Json::object(), frontend::FlushUrgency::Deferred})
                .accepted(),
            "bounded coalescer accepts its first dirty entity");
        const auto overflow = bounded.mark(
            {frontend::CoalescingKey::thread("two"), "thread.updated", frontend::Json::object(), frontend::FlushUrgency::Deferred});
        result.expectTrue(overflow.status == frontend::CoalescerMarkStatus::SnapshotRequired && bounded.dirtyCount() == 1,
                          "dirty-entity capacity is bounded and degrades to a snapshot instead of growing");
    }

    void testAuthenticationAdmissionAndTopology(tests::support::TestResult& result) {
        const auto transport = std::make_shared<tests::codex::FakeTransportState>();
        ManualScheduler scheduler;
        ManualTimerScheduler timers;
        std::uint64_t clockMs = 1000;
        backend::BackendCoreOptions backendOptions;
        backendOptions.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        FakeBackendCore core(backendOptions, transport);

        frontend::FrontendServiceOptions options;
        options.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        options.timerScheduler = [&timers](std::uint64_t delayMs, std::function<void()> callback) {
            return timers.schedule(delayMs, std::move(callback));
        };
        options.monotonicClockMs = [&clockMs] {
            return clockMs;
        };
        options.maximumFailedAuthenticationsPerPeer = 3;
        options.failedAuthenticationWindowMs = 60000;
        options.authenticator = [](const frontend::FrontendPeerContext&, const frontend::AuthenticationCredential& credential) {
            if (const auto* bearer = std::get_if<frontend::BearerCredential>(&credential); bearer && bearer->token == "test-token") {
                return frontend::AuthenticationResult{frontend::AuthenticationSuccess{frontend::FrontendPrincipal{
                    "remote-test",
                    std::vector<frontend::FrontendScope>{frontend::FrontendScope::Observe, frontend::FrontendScope::Control},
                    "default_remote",
                    false}}};
            }
            return frontend::AuthenticationResult{frontend::AuthenticationFailure{
                std::holds_alternative<frontend::NoCredential>(credential) ? frontend::AuthenticationFailureCode::AuthenticationRequired
                                                                           : frontend::AuthenticationFailureCode::AuthenticationFailed}};
        };
        frontend::FrontendService service(core, options);

        frontend::FrontendServiceOptions localOptions = options;
        localOptions.authenticator = {};
        localOptions.trustedLocalUserId = TrustedTestUserId;
        frontend::FrontendService localService(core, localOptions);
        Observations verifiedLocal;
        frontend::FrontendConnection verifiedLocalConnection = localService.openConnection(trustedPeer(), callbacksFor(verifiedLocal));
        result.expectTrue(verifiedLocalConnection.receive(hello()).accepted(),
                          "verified same-user Unix trust accepts the original credential-free Hello");
        scheduler.drain();
        result.expectTrue(verifiedLocalConnection.principal().has_value() && verifiedLocalConnection.principal()->localTrusted &&
                              verifiedLocalConnection.principal()->profile == "local_trusted" && verifiedLocalConnection.peer().localPeer &&
                              verifiedLocalConnection.peer().unixUserId == TrustedTestUserId,
                          "verified peer identity and local_trusted principal are visible through credential-free diagnostics");

        frontend::FrontendPeerContext wrongUnixPeer = trustedPeer();
        wrongUnixPeer.unixUserId = TrustedTestUserId + 1;
        Observations wrongUnix;
        frontend::FrontendConnection wrongUnixConnection = localService.openConnection(wrongUnixPeer, callbacksFor(wrongUnix));
        result.expectTrue(wrongUnixConnection.receive(hello()).status == frontend::ConnectionReceiveStatus::Closing,
                          "a wrong verified Unix peer identity does not inherit local trust");
        scheduler.drain();
        frontend::FrontendPeerContext missingUnixCredentials = trustedPeer();
        missingUnixCredentials.localPeer = false;
        missingUnixCredentials.unixUserId.reset();
        Observations missingUnix;
        frontend::FrontendConnection missingUnixConnection = localService.openConnection(missingUnixCredentials, callbacksFor(missingUnix));
        result.expectTrue(missingUnixConnection.receive(hello()).status == frontend::ConnectionReceiveStatus::Closing,
                          "missing Unix peer credentials never silently become trusted");
        scheduler.drain();

        frontend::FrontendServiceOptions overrideOptions = localOptions;
        overrideOptions.allowInsecureLocalTrust = true;
        frontend::FrontendService overrideService(core, overrideOptions);
        Observations overriddenLocal;
        frontend::FrontendConnection overriddenConnection =
            overrideService.openConnection(missingUnixCredentials, callbacksFor(overriddenLocal));
        result.expectTrue(overriddenConnection.receive(hello()).accepted(),
                          "the explicit insecure Unix trust override is the only credential-free fallback");
        scheduler.drain();
        result.expectTrue(overriddenConnection.principal().has_value() && overriddenConnection.principal()->localTrusted &&
                              overriddenConnection.principal()->id == "insecure-local-override",
                          "insecure override remains structurally distinguishable from verified local trust");

        frontend::FrontendPeerContext remote;
        remote.transport = frontend::FrontendTransportKind::Ipv4;
        remote.loopback = true;
        remote.remoteAddress = "127.0.0.1";

        const std::size_t sessionsBeforeMissingAuthentication = core.snapshot().sessions.size();
        Observations missing;
        frontend::FrontendConnection missingConnection = service.openConnection(remote, callbacksFor(missing));
        result.expectTrue(missingConnection.isOpen() && service.unauthenticatedConnectionCount() == 1 &&
                              service.authenticatedConnectionCount() == 0 && !missingConnection.sessionId().has_value(),
                          "opening a transport connection consumes only unauthenticated admission capacity");
        result.expectTrue(missingConnection.receive(hello()).status == frontend::ConnectionReceiveStatus::Closing,
                          "a remote Hello without a bearer credential is rejected terminally");
        result.expectTrue(!missingConnection.sessionId().has_value() &&
                              core.snapshot().sessions.size() == sessionsBeforeMissingAuthentication,
                          "failed authentication creates no BackendCore session or controller state");
        scheduler.drain();
        result.expectTrue(!missingConnection.isOpen() && protocolError(missing, std::nullopt) != nullptr &&
                              protocolError(missing, std::nullopt)->code == frontend::ErrorCode::AuthenticationRequired,
                          "missing authentication emits one bounded authentication_required error and closes");

        const auto badHello =
            frontend::ClientMessage{frontend::Hello{std::nullopt,
                                                    frontend::Json::object(),
                                                    std::nullopt,
                                                    frontend::AuthenticationCredential{frontend::BearerCredential{"wrong-token"}}}};
        for (std::size_t attempt = 0; attempt < 2; ++attempt) {
            Observations failed;
            frontend::FrontendConnection connection = service.openConnection(remote, callbacksFor(failed));
            result.expectTrue(connection.receive(badHello).status == frontend::ConnectionReceiveStatus::Closing,
                              "a wrong bearer consumes exactly one peer authentication attempt");
            scheduler.drain();
            result.expectTrue(protocolError(failed, std::nullopt) != nullptr &&
                                  protocolError(failed, std::nullopt)->code == frontend::ErrorCode::AuthenticationFailed,
                              "wrong bearer rejection does not expose credential details");
        }
        Observations limited;
        frontend::FrontendConnection limitedConnection = service.openConnection(remote, callbacksFor(limited));
        result.expectTrue(limitedConnection.receive(badHello).status == frontend::ConnectionReceiveStatus::Closing,
                          "the fourth peer attempt is terminal after three recorded failures");
        scheduler.drain();
        result.expectTrue(protocolError(limited, std::nullopt) != nullptr &&
                              protocolError(limited, std::nullopt)->code == frontend::ErrorCode::RateLimited,
                          "the failed-authentication peer budget returns rate_limited without invoking BackendCore");

        frontend::FrontendPeerContext otherRemote = remote;
        otherRemote.remoteAddress = "127.0.0.2";
        Observations authenticated;
        frontend::FrontendConnection authenticatedConnection = service.openConnection(otherRemote, callbacksFor(authenticated));
        const auto goodHello =
            frontend::ClientMessage{frontend::Hello{std::nullopt,
                                                    frontend::Json::object(),
                                                    std::vector{frontend::FrontendCapability::MethodDiscovery},
                                                    frontend::AuthenticationCredential{frontend::BearerCredential{"test-token"}}}};
        result.expectTrue(authenticatedConnection.receive(goodHello).accepted(), "a valid bearer authenticates another peer");
        scheduler.drain();
        result.expectTrue(authenticatedConnection.helloComplete() && authenticatedConnection.sessionId().has_value() &&
                              authenticatedConnection.principal().has_value() &&
                              authenticatedConnection.principal()->profile == "default_remote" &&
                              service.authenticatedConnectionCount() == 1 && service.unauthenticatedConnectionCount() == 0,
                          "authentication precedes BackendCore session creation and exposes only safe principal diagnostics");

        service.declareTransportFamily(frontend::FrontendTransportKind::Unix);
        const std::vector<frontend::FrontendCapability> singleFamilyCapabilities = service.implementedCapabilities();
        service.declareTransportFamily(frontend::FrontendTransportKind::Ipv4);
        const std::vector<frontend::FrontendCapability> multiFamilyCapabilities = service.implementedCapabilities();
        result.expectTrue(
            std::find(singleFamilyCapabilities.begin(), singleFamilyCapabilities.end(), frontend::FrontendCapability::MultiTransport) ==
                    singleFamilyCapabilities.end() &&
                std::find(multiFamilyCapabilities.begin(), multiFamilyCapabilities.end(), frontend::FrontendCapability::MultiTransport) !=
                    multiFamilyCapabilities.end(),
            "multi_transport follows distinct successfully declared listener families, not active connections");
        const frontend::Welcome* firstWelcome = welcome(authenticated);
        result.expectTrue(firstWelcome && firstWelcome->capabilities &&
                              std::find(firstWelcome->capabilities->implemented.begin(),
                                        firstWelcome->capabilities->implemented.end(),
                                        frontend::FrontendCapability::MultiTransport) == firstWelcome->capabilities->implemented.end(),
                          "an existing connection keeps the topology capability advertised during its handshake");

        Observations timedOut;
        frontend::FrontendConnection timeoutConnection = service.openConnection(otherRemote, callbacksFor(timedOut));
        const std::size_t timeoutIndex = timers.entries.size() - 1;
        result.expectTrue(timers.entries[timeoutIndex].delayMs == 10000 && timers.fire(timeoutIndex),
                          "the configured handshake deadline uses the injected event-loop timer seam");
        scheduler.drain();
        result.expectTrue(!timeoutConnection.isOpen() && !timeoutConnection.sessionId().has_value() &&
                              protocolError(timedOut, std::nullopt) != nullptr &&
                              protocolError(timedOut, std::nullopt)->code == frontend::ErrorCode::AuthenticationRequired,
                          "handshake timeout closes only the unauthenticated connection without a BackendCore session");

        frontend::FrontendServiceOptions capacityOptions = options;
        capacityOptions.maxUnauthenticatedConnections = 1;
        capacityOptions.maxConnections = 1;
        frontend::FrontendService capacityService(core, capacityOptions);
        Observations capacityA;
        Observations capacityB;
        frontend::FrontendConnection admitted = capacityService.openConnection(otherRemote, callbacksFor(capacityA));
        frontend::FrontendConnection rejected = capacityService.openConnection(otherRemote, callbacksFor(capacityB));
        result.expectTrue(admitted.isOpen() && !rejected.isOpen() && capacityService.connectionCount() == 1,
                          "connection and unauthenticated admission reject only the excess connection");

        frontend::FrontendServiceOptions outstandingOptions = options;
        outstandingOptions.maxOutstandingCommandsPerConnection = 1;
        frontend::FrontendService outstandingService(core, outstandingOptions);
        Observations outstandingObservations;
        frontend::FrontendConnection outstandingConnection =
            outstandingService.openConnection(otherRemote, callbacksFor(outstandingObservations));
        result.expectTrue(outstandingConnection.receive(goodHello).accepted(), "outstanding-command test authenticates normally");
        scheduler.drain();
        result.expectTrue(outstandingConnection.receive(command("pending-one", frontend::ControllerAcquire{})).accepted() &&
                              outstandingConnection.receive(command("pending-two", frontend::ControllerAcquire{})).status ==
                                  frontend::ConnectionReceiveStatus::Rejected,
                          "one pending request consumes the configured outstanding-command capacity");
        scheduler.drain();
        result.expectTrue(responseHasError(outstandingObservations, "pending-two", frontend::ErrorCode::CapacityExceeded),
                          "outstanding-command overflow returns capacity_exceeded without affecting the service");

        frontend::FrontendServiceOptions rateOptions = options;
        rateOptions.maxInboundBurst = 1;
        rateOptions.maxInboundMessagesPerSecond = 1;
        frontend::FrontendService rateService(core, rateOptions);
        Observations rateObservations;
        frontend::FrontendConnection rateConnection = rateService.openConnection(otherRemote, callbacksFor(rateObservations));
        result.expectTrue(rateConnection.receive(goodHello).accepted(), "the initial rate token admits Hello");
        result.expectTrue(rateConnection.receive(command("too-fast", frontend::SnapshotGet{})).status ==
                              frontend::ConnectionReceiveStatus::Closing,
                          "the next message without token refill is rate limited deterministically");
        scheduler.drain();
        result.expectTrue(!rateConnection.isOpen(), "rate-limit closure is connection-local and reusable service state remains valid");

        frontend::FrontendServiceOptions frameOptions = options;
        frameOptions.maximumInboundMessageBytes = 8;
        frontend::FrontendService frameService(core, frameOptions);
        Observations frameObservations;
        frontend::FrontendConnection frameConnection = frameService.openConnection(otherRemote, callbacksFor(frameObservations));
        result.expectTrue(frameConnection.receive(std::string_view{"{\"oversized\":true}"}).status ==
                              frontend::ConnectionReceiveStatus::Closing,
                          "the transport-neutral frame bound rejects input before decoding or authentication");
        scheduler.drain();
        result.expectTrue(protocolError(frameObservations, std::nullopt) != nullptr &&
                              protocolError(frameObservations, std::nullopt)->code == frontend::ErrorCode::FrameTooLarge,
                          "oversized input reports frame_too_large without method-policy disclosure");

        frontend::FrontendServiceOptions preAuthenticationOptions = options;
        frontend::FrontendService preAuthenticationService(core, preAuthenticationOptions);
        Observations preAuthenticationObservations;
        frontend::FrontendConnection preAuthenticationConnection =
            preAuthenticationService.openConnection(otherRemote, callbacksFor(preAuthenticationObservations));
        result.expectTrue(preAuthenticationConnection
                                  .receive(frontend::Json{{"protocol", frontend::ProtocolIdentity},
                                                          {"version", frontend::ProtocolVersion},
                                                          {"kind", "command"},
                                                          {"requestId", "pre-auth-sensitive"},
                                                          {"method", "command.exec"},
                                                          {"params", frontend::Json{{"credential", "synthetic-secret-sentinel"}}}})
                                  .status == frontend::ConnectionReceiveStatus::Closing,
                          "pre-authentication command decoding is terminal without policy inspection");
        scheduler.drain();
        const frontend::ProtocolErrorMessage* preAuthenticationError = protocolError(preAuthenticationObservations, std::nullopt);
        result.expectTrue(preAuthenticationError != nullptr &&
                              preAuthenticationError->code == frontend::ErrorCode::AuthenticationRequired &&
                              preAuthenticationError->message == "frontend authentication must complete before commands are accepted" &&
                              preAuthenticationError->requestId == std::nullopt &&
                              preAuthenticationObservations.compactJson.end() ==
                                  std::find_if(preAuthenticationObservations.compactJson.begin(),
                                               preAuthenticationObservations.compactJson.end(),
                                               [](const std::string& encoded) {
                                                   return encoded.find("command.exec") != std::string::npos ||
                                                          encoded.find("synthetic-secret-sentinel") != std::string::npos;
                                               }),
                          "pre-authentication errors expose neither method existence, request correlation, nor parameter data");

        frontend::FrontendServiceOptions accountingOptions = options;
        accountingOptions.maxConnections = 1;
        frontend::FrontendService accountingService(core, accountingOptions);
        Observations firstPeerFailure;
        frontend::FrontendConnection firstPeer = accountingService.openConnection(remote, callbacksFor(firstPeerFailure));
        result.expectTrue(firstPeer.receive(badHello).status == frontend::ConnectionReceiveStatus::Closing,
                          "the bounded failed-peer table records its first address");
        scheduler.drain();
        Observations secondPeerFailure;
        frontend::FrontendConnection secondPeer = accountingService.openConnection(otherRemote, callbacksFor(secondPeerFailure));
        result.expectTrue(secondPeer.receive(badHello).status == frontend::ConnectionReceiveStatus::Closing,
                          "a new address is rejected when failed-peer accounting is full");
        scheduler.drain();
        result.expectTrue(protocolError(secondPeerFailure, std::nullopt) != nullptr &&
                              protocolError(secondPeerFailure, std::nullopt)->code == frontend::ErrorCode::RateLimited,
                          "failed-authentication peer accounting remains bounded by service connection capacity");

        accountingService.close();
        preAuthenticationService.close();
        frameService.close();
        rateService.close();
        outstandingService.close();
        capacityService.close();
        overrideService.close();
        localService.close();
        service.close();
        scheduler.drain();
    }

    void testServiceHandshakeRolesReplayAndIsolation(tests::support::TestResult& result) {
        const auto transport = std::make_shared<tests::codex::FakeTransportState>();
        ManualScheduler scheduler;
        backend::BackendCoreOptions backendOptions;
        backendOptions.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        backendOptions.maxEventsPerCallback = 128;
        FakeBackendCore core(backendOptions, transport);

        frontend::FrontendServiceOptions serviceOptions;
        enableVerifiedTestTrust(serviceOptions);
        serviceOptions.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        serviceOptions.journal = {8, 128U * 1024U, frontend::SequenceNumber{0}};
        serviceOptions.batches = {8, 32U * 1024U};
        serviceOptions.maxOutboundMessagesPerConnection = 64;
        serviceOptions.maxOutboundBytesPerConnection = 512U * 1024U;
        frontend::FrontendService service(core, serviceOptions);

        Observations observerA;
        Observations observerB;
        frontend::FrontendConnection connectionA = service.openConnection(trustedPeer(), callbacksFor(observerA));
        frontend::FrontendConnection connectionB = service.openConnection(trustedPeer(), callbacksFor(observerB));
        result.expectTrue(connectionA.receive(hello()).accepted() && connectionB.receive(hello()).accepted() &&
                              observerA.messages.empty() && observerB.messages.empty(),
                          "hello output is asynchronous for every transport-neutral connection");
        scheduler.drain();

        result.expectTrue(welcome(observerA) && welcome(observerA)->role == frontend::SessionRole::Observer &&
                              welcome(observerA)->syncMode == frontend::SyncMode::Snapshot && countSnapshots(observerA) == 1 &&
                              welcome(observerB) && welcome(observerB)->role == frontend::SessionRole::Observer,
                          "hello creates backend sessions as observers and completes initial snapshot synchronization");
        result.expectTrue(connectionA.helloComplete() && connectionB.helloComplete() && connectionA.sessionId() != connectionB.sessionId(),
                          "successful hello exposes stable distinct backend session IDs");

        const frontend::SequenceNumber journalSequenceBeforeReplay = service.currentSequence();
        backend::FrontendSession replayMutationSession = core.openSession({});
        scheduler.drain();
        const frontend::SequenceNumber journalSequenceAfterMutation = service.currentSequence();
        const backend::SequenceNumber backendSequenceBeforeReplay = core.snapshot().sequence;
        const std::size_t messagesBeforeReplay = observerA.messages.size();
        result.expectTrue(connectionA.receive(command("journal-replay", frontend::ReplayAfter{journalSequenceBeforeReplay})).accepted(),
                          "events.replay is accepted as a frontend-journal command");
        scheduler.drain();
        const frontend::Response* journalReplayResponse = response(observerA, "journal-replay");
        std::optional<std::size_t> replayResponseIndex;
        std::optional<std::size_t> replayEventsIndex;
        std::optional<std::size_t> replayCompleteIndex;
        for (std::size_t index = messagesBeforeReplay; index < observerA.messages.size(); ++index) {
            const frontend::ServerMessage& message = observerA.messages[index];
            if (const auto* value = std::get_if<frontend::Response>(&message); value && value->requestId == "journal-replay") {
                replayResponseIndex = index;
            } else if (std::holds_alternative<frontend::EventBatch>(message)) {
                replayEventsIndex = index;
            } else if (const auto* complete = std::get_if<frontend::SyncComplete>(&message);
                       complete && complete->sequence == journalSequenceAfterMutation) {
                replayCompleteIndex = index;
            }
        }
        result.expectTrue(journalReplayResponse && journalReplayResponse->ok && journalReplayResponse->result &&
                              journalReplayResponse->result->value("syncMode", "") == "replay" &&
                              journalReplayResponse->result->value("sequence", std::uint64_t{0}) == journalSequenceAfterMutation.value() &&
                              replayResponseIndex && replayEventsIndex && replayCompleteIndex &&
                              *replayResponseIndex < *replayEventsIndex && *replayEventsIndex < *replayCompleteIndex &&
                              journalSequenceAfterMutation > journalSequenceBeforeReplay &&
                              core.snapshot().sequence == backendSequenceBeforeReplay,
                          "events.replay returns response, retained frontend events, then sync.complete without a BackendCore transition");

        const std::size_t messagesBeforeCommands = observerA.messages.size();
        const std::size_t liveEventBaselineA = events(observerA).size();
        const std::size_t liveEventBaselineB = events(observerB).size();
        result.expectTrue(connectionA.receive(command("acquire-a", frontend::ControllerAcquire{})).accepted(),
                          "observer A submits explicit controller acquisition");
        result.expectTrue(!connectionA.receive(command("acquire-a", frontend::ControllerAcquire{})).accepted(),
                          "duplicate still-pending requestId is rejected locally");
        result.expectTrue(connectionB.receive(command("observer-mutate", frontend::ThreadStart{})).status ==
                              frontend::ConnectionReceiveStatus::Rejected,
                          "observer mutation is denied before BackendCore submission with one correlated response");
        scheduler.drain();
        result.expectTrue(hasSuccessfulResponse(observerA, "acquire-a"),
                          "controller acquisition completes successfully without duplicate backend completion");
        std::size_t duplicateResponses = 0;
        for (const frontend::ServerMessage& message : observerA.messages) {
            if (const auto* value = std::get_if<frontend::Response>(&message);
                value && value->requestId == "acquire-a" && !value->ok && value->error &&
                value->error->code == frontend::ErrorCode::DuplicateRequestId) {
                ++duplicateResponses;
            }
        }
        result.expectTrue(duplicateResponses == 1 && responseHasError(observerB, "observer-mutate", frontend::ErrorCode::PermissionDenied),
                          "duplicate correlation and observer permission errors use stable isolated responses");
        result.expectTrue(observerA.messages.size() > messagesBeforeCommands,
                          "controller transition produces frontend-visible normalized output");

        const std::string secretSentinel = "frontend-auth-secret-must-not-leak";
        result.expectTrue(
            connectionA.receive(command("auth-secret", frontend::AuthenticationRespond{"999", secretSentinel, "account", "plus"})).status ==
                frontend::ConnectionReceiveStatus::Rejected,
            "reverse responses require provider readiness without exposing their secret in the server contract");
        scheduler.drain();
        const bool secretLeaked =
            std::any_of(observerA.compactJson.begin(), observerA.compactJson.end(), [&secretSentinel](const auto& json) {
                return json.find(secretSentinel) != std::string::npos;
            });
        result.expectTrue(responseHasError(observerA, "auth-secret", frontend::ErrorCode::BackendUnavailable) && !secretLeaked,
                          "server output never serializes an authentication access token from a frontend command");

        const std::vector<frontend::FrontendEvent> allEventsA = events(observerA);
        const std::vector<frontend::FrontendEvent> allEventsB = events(observerB);
        const std::vector<frontend::FrontendEvent> eventsA{allEventsA.begin() + static_cast<std::ptrdiff_t>(liveEventBaselineA),
                                                           allEventsA.end()};
        const std::vector<frontend::FrontendEvent> eventsB{allEventsB.begin() + static_cast<std::ptrdiff_t>(liveEventBaselineB),
                                                           allEventsB.end()};
        bool ordered = true;
        for (std::size_t index = 1; index < eventsA.size(); ++index) {
            ordered = ordered && eventsA[index - 1].sequence < eventsA[index].sequence;
        }
        result.expectTrue(!eventsA.empty() && !eventsB.empty() && ordered,
                          "multiple observers receive normalized frontend batches in strict sequence order");

        const frontend::SequenceNumber resumePosition = service.currentSequence();
        connectionA.close("controller A disconnected");
        scheduler.drain();
        result.expectTrue(connectionB.isOpen(), "controller disconnect leaves another observer and BackendCore running");

        Observations replayed;
        frontend::FrontendConnection replayConnection = service.openConnection(trustedPeer(), callbacksFor(replayed));
        result.expectTrue(replayConnection.receive(hello(resumePosition)).accepted(),
                          "a reconnect may request replay after its last sequence");
        scheduler.drain();
        result.expectTrue(welcome(replayed) && welcome(replayed)->syncMode == frontend::SyncMode::Replay && countSnapshots(replayed) == 0 &&
                              !events(replayed).empty(),
                          "retained normalized frontend events replay without a redundant snapshot");

        // Generate more separately flushed controller transitions than the
        // configured journal can retain, then reconnect from sequence zero.
        result.expectTrue(connectionB.receive(command("acquire-b", frontend::ControllerAcquire{})).accepted(),
                          "observer B acquires controller after A disconnects");
        scheduler.drain();
        for (std::size_t index = 0; index < 6; ++index) {
            result.expectTrue(connectionB.receive(command("release-" + std::to_string(index), frontend::ControllerRelease{})).accepted(),
                              "controller release command is accepted during eviction setup");
            scheduler.drain();
            result.expectTrue(connectionB.receive(command("acquire-" + std::to_string(index), frontend::ControllerAcquire{})).accepted(),
                              "controller reacquisition command is accepted during eviction setup");
            scheduler.drain();
        }

        const backend::SequenceNumber backendSequenceBeforeGapReplay = core.snapshot().sequence;
        const std::size_t messagesBeforeGapReplay = observerB.messages.size();
        result.expectTrue(connectionB.receive(command("gap-replay", frontend::ReplayAfter{frontend::SequenceNumber{0}})).accepted(),
                          "events.replay accepts an evicted frontend sequence for snapshot fallback");
        scheduler.drain();
        const frontend::Response* gapReplayResponse = response(observerB, "gap-replay");
        std::optional<std::size_t> gapResponseIndex;
        std::optional<std::size_t> gapSnapshotIndex;
        std::optional<std::size_t> gapCompleteIndex;
        bool gapEmittedEvents = false;
        for (std::size_t index = messagesBeforeGapReplay; index < observerB.messages.size(); ++index) {
            const frontend::ServerMessage& message = observerB.messages[index];
            if (const auto* value = std::get_if<frontend::Response>(&message); value && value->requestId == "gap-replay") {
                gapResponseIndex = index;
            } else if (std::holds_alternative<frontend::Snapshot>(message)) {
                gapSnapshotIndex = index;
            } else if (std::holds_alternative<frontend::EventBatch>(message)) {
                gapEmittedEvents = true;
            } else if (const auto* complete = std::get_if<frontend::SyncComplete>(&message);
                       complete && complete->sequence == service.currentSequence()) {
                gapCompleteIndex = index;
            }
        }
        result.expectTrue(gapReplayResponse && gapReplayResponse->ok && gapReplayResponse->result &&
                              gapReplayResponse->result->value("syncMode", "") == "snapshot" && gapResponseIndex && gapSnapshotIndex &&
                              gapCompleteIndex && *gapResponseIndex < *gapSnapshotIndex && *gapSnapshotIndex < *gapCompleteIndex &&
                              !gapEmittedEvents && core.snapshot().sequence == backendSequenceBeforeGapReplay,
                          "events.replay gap returns response, one snapshot, then sync.complete without a BackendCore transition");

        const backend::SequenceNumber backendSequenceBeforeFutureReplay = core.snapshot().sequence;
        const std::size_t messagesBeforeFutureReplay = observerB.messages.size();
        const frontend::SequenceNumber futureSequence{service.currentSequence().value() + 1};
        result.expectTrue(connectionB.receive(command("future-replay", frontend::ReplayAfter{futureSequence})).status ==
                              frontend::ConnectionReceiveStatus::Rejected,
                          "events.replay rejects a future frontend sequence");
        scheduler.drain();
        const bool futureReplayPayload = std::any_of(observerB.messages.begin() + static_cast<std::ptrdiff_t>(messagesBeforeFutureReplay),
                                                     observerB.messages.end(),
                                                     [](const frontend::ServerMessage& message) {
                                                         return std::holds_alternative<frontend::Snapshot>(message) ||
                                                                std::holds_alternative<frontend::EventBatch>(message) ||
                                                                std::holds_alternative<frontend::SyncComplete>(message);
                                                     });
        result.expectTrue(responseHasError(observerB, "future-replay", frontend::ErrorCode::InvalidCommand) && !futureReplayPayload &&
                              connectionB.isOpen() && core.snapshot().sequence == backendSequenceBeforeFutureReplay,
                          "future events.replay returns invalid_command without synchronization payload or BackendCore transition");

        Observations snapshotFallback;
        frontend::FrontendConnection oldReconnect = service.openConnection(trustedPeer(), callbacksFor(snapshotFallback));
        result.expectTrue(oldReconnect.receive(hello(frontend::SequenceNumber{0})).accepted(),
                          "an old reconnect position is accepted for synchronization planning");
        scheduler.drain();
        result.expectTrue(welcome(snapshotFallback) && welcome(snapshotFallback)->syncMode == frontend::SyncMode::Snapshot &&
                              countSnapshots(snapshotFallback) == 1,
                          "journal eviction deterministically falls back to one complete snapshot");

        Observations badClient;
        frontend::FrontendConnection beforeHello = service.openConnection(trustedPeer(), callbacksFor(badClient));
        result.expectTrue(beforeHello.receive(command("too-early", frontend::SnapshotGet{})).status ==
                              frontend::ConnectionReceiveStatus::Closing,
                          "a command before hello is rejected and closes only that frontend after its error");
        result.expectTrue(beforeHello.receive(hello()).status == frontend::ConnectionReceiveStatus::Closing,
                          "a connection pending protocol-error close rejects later coalesced input before opening a backend session");
        scheduler.drain();
        result.expectTrue(!beforeHello.isOpen() && connectionB.isOpen(), "pre-hello protocol failure is isolated from healthy clients");

        Observations slow;
        frontend::FrontendConnection slowObserver = service.openConnection(trustedPeer(),
                                                                           {[&slow](const frontend::OutboundMessage& message) {
                                                                                slow.messages.push_back(message.message);
                                                                                return false;
                                                                            },
                                                                            [&slow](const std::string& reason) {
                                                                                slow.closeReasons.push_back(reason);
                                                                            }});
        result.expectTrue(slowObserver.receive(hello()).accepted(), "slow observer starts handshake independently");
        scheduler.drain();
        result.expectTrue(!slowObserver.isOpen() && connectionB.isOpen() && connectionB.queuedMessages() == 0 &&
                              slowObserver.queuedMessages() == 0,
                          "a backpressured observer is disconnected and releases all queued data without growing the controller queue");

        Observations throwing;
        frontend::FrontendConnection throwingObserver =
            service.openConnection(trustedPeer(),
                                   {[&throwing](const frontend::OutboundMessage&) -> bool {
                                        throwing.messages.emplace_back(frontend::ProtocolErrorMessage{});
                                        throw std::runtime_error("intentional frontend transport failure");
                                    },
                                    [&throwing](const std::string& reason) {
                                        throwing.closeReasons.push_back(reason);
                                    }});
        result.expectTrue(throwingObserver.receive(hello()).accepted(), "throwing observer starts handshake independently");
        scheduler.drain();
        result.expectTrue(!throwingObserver.isOpen() && connectionB.isOpen(),
                          "a throwing outbound callback is exception-bounded and does not suppress another observer");

        Observations selfClosing;
        std::optional<frontend::FrontendConnection> selfClosingConnection;
        selfClosingConnection.emplace(service.openConnection(trustedPeer(),
                                                             {[&selfClosingConnection](const frontend::OutboundMessage&) {
                                                                  selfClosingConnection->close("closed during delivery");
                                                                  return true;
                                                              },
                                                              [&selfClosing](const std::string& reason) {
                                                                  selfClosing.closeReasons.push_back(reason);
                                                              }}));
        result.expectTrue(selfClosingConnection->receive(hello()).accepted(), "frontend may request close during outbound delivery");
        scheduler.drain();
        result.expectTrue(!selfClosingConnection->isOpen() && connectionB.isOpen(),
                          "close during callback delivery is generation/lifetime safe and peer-isolated");

        replayMutationSession.close();
        scheduler.drain();
        service.close("service test complete");
        scheduler.drain();
        result.expectTrue(!service.isOpen() && !connectionB.isOpen() && !replayConnection.isOpen() && !oldReconnect.isOpen(),
                          "service shutdown detaches every frontend without destroying BackendCore");
    }

    void testCapabilityDiscoveryHandshake(tests::support::TestResult& result) {
        const auto transport = std::make_shared<tests::codex::FakeTransportState>();
        ManualScheduler scheduler;
        backend::BackendCoreOptions backendOptions;
        backendOptions.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        FakeBackendCore core(backendOptions, transport);

        frontend::FrontendServiceOptions serviceOptions;
        enableVerifiedTestTrust(serviceOptions);
        serviceOptions.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        frontend::FrontendService service(core, serviceOptions);

        Observations observations;
        frontend::FrontendConnection connection = service.openConnection(trustedPeer(), callbacksFor(observations));
        const frontend::Hello discoveryHello{
            std::nullopt,
            frontend::Json::object(),
            std::vector{frontend::FrontendCapability::MethodDiscovery, frontend::FrontendCapability::SecurityScopes},
        };
        result.expectTrue(connection.receive(frontend::ClientMessage{discoveryHello}).accepted() && observations.messages.empty(),
                          "capability-aware hello preserves asynchronous handshake delivery");
        scheduler.drain();

        const frontend::Welcome* discovery = welcome(observations);
        result.expectTrue(discovery != nullptr && discovery->capabilities.has_value() && discovery->capabilities->defined.size() == 18 &&
                              discovery->capabilities->implemented.size() == 13 &&
                              discovery->capabilities->permitted == discovery->capabilities->implemented,
                          "A1.7b Welcome distinguishes 18 definitions from thirteen implemented service mechanisms");
        result.expectTrue(discovery != nullptr && discovery->availableMethods.has_value() && discovery->availableMethods->size() == 90 &&
                              discovery->permittedMethods == discovery->availableMethods &&
                              std::all_of(discovery->availableMethods->begin(),
                                          discovery->availableMethods->end(),
                                          [](const frontend::FrontendMethod& method) {
                                              return frontend::generated::runtimeMethodFromString(method).has_value();
                                          }),
                          "the local_trusted A1.7b runtime advertises and permits all 90 deployment-enabled methods");

        const std::size_t providerSubmissions = transport->outgoing.size();
        const frontend::ConnectionReceiveResult unavailable = connection.receive(frontend::Json{
            {"protocol", frontend::ProtocolIdentity},
            {"version", frontend::ProtocolVersion},
            {"kind", "command"},
            {"requestId", "defined-but-unavailable"},
            {"method", "fs.readFile"},
            {"params", frontend::Json{{"path", 7}}},
        });
        result.expectTrue(unavailable.status == frontend::ConnectionReceiveStatus::Rejected,
                          "a defined but deployment-disabled method is rejected before its parameter schema is inspected");
        scheduler.drain();
        const frontend::ProtocolErrorMessage* unavailableError = protocolError(observations, "defined-but-unavailable");
        result.expectTrue(unavailableError != nullptr && unavailableError->code == frontend::ErrorCode::UnknownMethod &&
                              !unavailableError->closeConnection && connection.isOpen() && observations.closeReasons.empty(),
                          "deployment-disabled methods return unknown_method without closing the established connection");
        result.expectTrue(transport->outgoing.size() == providerSubmissions, "disabled conditional methods never reach BackendCore");
    }

    void testSnapshotReplayBarrier(tests::support::TestResult& result) {
        const auto transport = std::make_shared<tests::codex::FakeTransportState>();
        ManualScheduler scheduler;
        backend::BackendCoreOptions backendOptions;
        backendOptions.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        FakeBackendCore core(backendOptions, transport);

        frontend::FrontendServiceOptions serviceOptions;
        enableVerifiedTestTrust(serviceOptions);
        serviceOptions.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        serviceOptions.coalescer = {1};
        frontend::FrontendService service(core, serviceOptions);

        Observations initial;
        frontend::FrontendConnection initialConnection = service.openConnection(trustedPeer(), callbacksFor(initial));
        result.expectTrue(initialConnection.receive(hello()).accepted(), "replay-barrier client completes an initial hello");
        scheduler.drain();

        const frontend::SequenceNumber unchangedSequence = service.currentSequence();
        result.expectTrue(initialConnection.receive(command("unchanged-snapshot", frontend::SnapshotGet{})).accepted(),
                          "an explicit unchanged-state snapshot request is accepted");
        scheduler.drain();
        result.expectTrue(service.currentSequence() == unchangedSequence && hasSuccessfulResponse(initial, "unchanged-snapshot"),
                          "an explicit snapshot does not advance or invalidate replay continuity");

        Observations unchangedReplay;
        frontend::FrontendConnection unchangedReplayConnection = service.openConnection(trustedPeer(), callbacksFor(unchangedReplay));
        result.expectTrue(unchangedReplayConnection.receive(hello(unchangedSequence)).accepted(),
                          "an unchanged-state reconnect requests replay at the current sequence");
        scheduler.drain();
        result.expectTrue(welcome(unchangedReplay) && welcome(unchangedReplay)->syncMode == frontend::SyncMode::Replay &&
                              countSnapshots(unchangedReplay) == 0,
                          "unchanged state remains replayable without a redundant snapshot");

        const frontend::SequenceNumber beforeFallback = service.currentSequence();
        backend::FrontendSession dirtySessionA = core.openSession({});
        backend::FrontendSession dirtySessionB = core.openSession({});
        scheduler.drain();
        result.expectTrue(service.currentSequence() > beforeFallback,
                          "dirty-entity overflow advances the frontend synchronization barrier monotonically");

        Observations staleReconnect;
        frontend::FrontendConnection staleConnection = service.openConnection(trustedPeer(), callbacksFor(staleReconnect));
        result.expectTrue(staleConnection.receive(hello(beforeFallback)).accepted(),
                          "a stale client may reconnect from the sequence immediately before snapshot fallback");
        scheduler.drain();
        result.expectTrue(welcome(staleReconnect) && welcome(staleReconnect)->syncMode == frontend::SyncMode::Snapshot &&
                              countSnapshots(staleReconnect) == 1,
                          "snapshot fallback establishes a replay gap even when no normalized event was journaled");

        staleConnection.close();
        unchangedReplayConnection.close();
        initialConnection.close();
        dirtySessionB.close();
        dirtySessionA.close();
        service.close("replay barrier test complete");
        scheduler.drain();
    }

    void testCapacityOnlySnapshotFeedback(tests::support::TestResult& result) {
        const auto transport = std::make_shared<tests::codex::FakeTransportState>();
        ManualScheduler scheduler;
        backend::BackendCoreOptions backendOptions;
        backendOptions.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        backendOptions.capacity.maxSnapshotBytes = 1;
        FakeBackendCore core(std::move(backendOptions), transport);

        frontend::FrontendServiceOptions serviceOptions;
        enableVerifiedTestTrust(serviceOptions);
        serviceOptions.scheduler = [&scheduler](std::function<void()> callback) {
            scheduler.schedule(std::move(callback));
        };
        frontend::FrontendService service(core, std::move(serviceOptions));

        const backend::Snapshot first = core.snapshot();
        scheduler.drain(64);
        const backend::BackendState accounted = core.state();
        result.expectTrue(first.capacity.truncated && first.capacity.mandatoryCoreExceedsLimit &&
                              accounted.capacity.snapshotOmissions == 1 && scheduler.pending() == 0,
                          "a capacity-only frontend batch cannot create a snapshot-omission feedback cycle");

        const backend::SequenceNumber sequence = accounted.sequence;
        const backend::Snapshot repeated = core.snapshot();
        scheduler.drain(64);
        const backend::BackendState stable = core.state();
        result.expectTrue(repeated.capacity.state.snapshotOmissions == 1 && stable.sequence == sequence &&
                              stable.capacity.snapshotOmissions == 1 && scheduler.pending() == 0,
                          "repeated snapshots at one canonical revision account mandatory-envelope omission only once");

        service.close("capacity feedback test complete");
        scheduler.drain();
    }

    class SparseSequenceRunner {
    public:
        explicit SparseSequenceRunner(tests::support::TestResult& result)
            : result(result) {
        }

        void start(std::function<void()> onFinished) {
            this->onFinished = std::move(onFinished);
            transport = std::make_shared<tests::codex::FakeTransportState>();
            tests::codex::installInitializingFake(transport, [this](const Json& message, const TransportCallbacks& callbacks) {
                const auto method = message.find("method");
                const auto id = message.find("id");
                if (method != message.end() && method->is_string() && *method == "thread/list" && id != message.end()) {
                    tests::codex::inject(
                        callbacks,
                        Json{{"id", *id}, {"result", {{"data", Json::array()}, {"nextCursor", nullptr}, {"backwardsCursor", nullptr}}}});
                }
            });

            backend::BackendCoreOptions options;
            options.initialThreadListLimit = 1;
            backendCore = std::make_unique<FakeBackendCore>(std::move(options), transport);
            backendCore->start();
            waitUntil(
                "sparse-sequence fake provider reaches Ready before service subscription",
                [this]() {
                    return backendCore->isReady();
                },
                [this]() {
                    startVisibleInterval();
                });
        }

        [[nodiscard]] bool isFinished() const noexcept {
            return finished;
        }

        [[nodiscard]] const std::string& waitingStage() const noexcept {
            return waitingDescription;
        }

    private:
        static void defer(std::function<void()> callback) {
            core::EventReceiver::atNextTick(std::move(callback));
        }

        void expect(bool condition, const std::string& message) {
            result.expectTrue(condition, message);
        }

        void
        waitUntil(std::string description, std::function<bool()> predicate, std::function<void()> next, std::size_t remaining = 8'000) {
            waitingDescription = description;
            defer([this,
                   description = std::move(description),
                   predicate = std::move(predicate),
                   next = std::move(next),
                   remaining]() mutable {
                if (finished) {
                    return;
                }
                try {
                    if (predicate()) {
                        waitingDescription = "advancing after: " + description;
                        next();
                    } else if (remaining == 0) {
                        expect(false, description);
                        finish();
                    } else {
                        waitUntil(std::move(description), std::move(predicate), std::move(next), remaining - 1);
                    }
                } catch (...) {
                    expect(false, "sparse-sequence runner contains callback exception at stage: " + description);
                    finish();
                }
            });
        }

        void afterTicks(std::size_t count, std::function<void()> next) {
            if (count == 0) {
                next();
                return;
            }
            defer([this, count, next = std::move(next)]() mutable {
                afterTicks(count - 1, std::move(next));
            });
        }

        frontend::FrontendServiceOptions serviceOptions(frontend::SequenceNumber initialSequence, std::size_t maxEntries) {
            frontend::FrontendServiceOptions options;
            enableVerifiedTestTrust(options);
            options.journal = {maxEntries, 512U * 1024U, initialSequence};
            options.batches = {16, 128U * 1024U};
            options.authenticator = [](const frontend::FrontendPeerContext&,
                                       const frontend::AuthenticationCredential& credential) -> frontend::AuthenticationResult {
                const auto* bearer = std::get_if<frontend::BearerCredential>(&credential);
                if (bearer == nullptr || bearer->token != SparseSequenceRemoteToken) {
                    return frontend::AuthenticationFailure{frontend::AuthenticationFailureCode::AuthenticationFailed};
                }
                return frontend::AuthenticationSuccess{frontend::FrontendPrincipal{
                    "sparse-default-remote",
                    std::vector<frontend::FrontendScope>{frontend::DefaultRemoteScopes.begin(), frontend::DefaultRemoteScopes.end()},
                    std::string(frontend::DefaultRemoteScopeProfile.name),
                    false}};
            };
            return options;
        }

        static frontend::FrontendPeerContext remotePeer() {
            frontend::FrontendPeerContext peer;
            peer.transport = frontend::FrontendTransportKind::Ipv4;
            peer.loopback = true;
            peer.remoteAddress = "127.0.0.143";
            return peer;
        }

        static frontend::ClientMessage remoteHello(std::optional<frontend::SequenceNumber> resumeAfter,
                                                   std::vector<frontend::FrontendCapability> capabilities = {}) {
            return frontend::Hello{resumeAfter,
                                   frontend::Json::object(),
                                   std::move(capabilities),
                                   frontend::AuthenticationCredential{frontend::BearerCredential{std::string(SparseSequenceRemoteToken)}}};
        }

        static std::set<std::uint64_t> messageSequences(const Observations& observations, std::size_t begin, std::size_t end) {
            std::set<std::uint64_t> sequences;
            end = std::min(end, observations.messages.size());
            for (std::size_t index = begin; index < end; ++index) {
                if (const auto* batch = std::get_if<frontend::EventBatch>(&observations.messages[index])) {
                    for (const frontend::FrontendEvent& event : batch->events) {
                        sequences.insert(event.sequence.value());
                    }
                }
            }
            return sequences;
        }

        static std::optional<std::size_t> firstSync(const Observations& observations, std::size_t begin = 0) {
            for (std::size_t index = begin; index < observations.messages.size(); ++index) {
                if (std::holds_alternative<frontend::SyncComplete>(observations.messages[index])) {
                    return index;
                }
            }
            return std::nullopt;
        }

        static bool hasBatchRange(const Observations& observations,
                                  std::size_t begin,
                                  std::size_t end,
                                  frontend::SequenceNumber from,
                                  frontend::SequenceNumber to) {
            end = std::min(end, observations.messages.size());
            for (std::size_t index = begin; index < end; ++index) {
                if (const auto* batch = std::get_if<frontend::EventBatch>(&observations.messages[index]);
                    batch && batch->fromSequence == from && batch->toSequence == to) {
                    return true;
                }
            }
            return false;
        }

        void injectVisibleFirst() {
            transport->inject({{"method", "configWarning"},
                               {"params",
                                {{"summary", "sparse visible first"},
                                 {"details", "visible details"},
                                 {"path", "/sparse/config.toml"},
                                 {"range", nullptr}}}});
        }

        void injectHidden(std::string payload) {
            transport->inject({{"method", "process/outputDelta"},
                               {"params",
                                {{"capReached", false},
                                 {"deltaBase64", std::move(payload)},
                                 {"processHandle", "sparse-process"},
                                 {"stream", "stdout"}}}});
        }

        void injectVisibleLast() {
            transport->inject(
                {{"method", "guardianWarning"}, {"params", {{"message", "sparse visible last"}, {"threadId", "sparse-thread"}}}});
        }

        void startVisibleInterval() {
            service = std::make_unique<frontend::FrontendService>(*backendCore, serviceOptions(frontend::SequenceNumber{37}, 32));
            local.emplace(service->openConnection(trustedPeer(), callbacksFor(localObservations)));
            const std::vector capabilities{frontend::FrontendCapability::DedicatedNotificationEvents,
                                           frontend::FrontendCapability::ScopeProjectedState};
            const frontend::Hello localHello{std::nullopt, frontend::Json::object(), capabilities, std::nullopt};
            expect(local->receive(frontend::ClientMessage{localHello}).accepted(),
                   "the local_trusted sparse-sequence observer authenticates");
            waitUntil(
                "local_trusted handshake completes after the initial provider projection",
                [this]() {
                    return local->helloComplete();
                },
                [this, capabilities]() {
                    expect(service->currentSequence() == frontend::SequenceNumber{39},
                           "the initial provider record and local_trusted session establish global cursor 39 (actual " +
                               std::to_string(service->currentSequence().value()) + ")");
                    remote.emplace(service->openConnection(remotePeer(), callbacksFor(remoteObservations)));
                    expect(remote->receive(remoteHello(std::nullopt, capabilities)).accepted(),
                           "the default_remote sparse-sequence observer authenticates");
                    waitUntil(
                        "default_remote handshake completes at the unchanged global cursor 40",
                        [this]() {
                            return remote->helloComplete();
                        },
                        [this]() {
                            expect(service->currentSequence() == frontend::SequenceNumber{40},
                                   "the default_remote session establishes global cursor 40 (actual " +
                                       std::to_string(service->currentSequence().value()) + ")");
                            localBaseline = localObservations.messages.size();
                            remoteBaseline = remoteObservations.messages.size();
                            injectVisibleFirst();
                            waitUntil(
                                "visible occurrence reaches global sequence 41",
                                [this]() {
                                    return service->currentSequence() == frontend::SequenceNumber{41};
                                },
                                [this]() {
                                    injectHidden("c3BhcnNlLWhpZGRlbg==");
                                    waitUntil(
                                        "privileged-only occurrence reaches global sequence 42",
                                        [this]() {
                                            return service->currentSequence() == frontend::SequenceNumber{42};
                                        },
                                        [this]() {
                                            injectVisibleLast();
                                            waitUntil(
                                                "second visible occurrence reaches global sequence 43",
                                                [this]() {
                                                    return service->currentSequence() == frontend::SequenceNumber{43};
                                                },
                                                [this]() {
                                                    afterTicks(16, [this]() {
                                                        verifyLiveAndReplay();
                                                    });
                                                });
                                        });
                                });
                        });
                });
        }

        void verifyLiveAndReplay() {
            std::set<std::uint64_t> localLive = messageSequences(localObservations, localBaseline, localObservations.messages.size());
            const std::set<std::uint64_t> remoteLive =
                messageSequences(remoteObservations, remoteBaseline, remoteObservations.messages.size());
            localLive.erase(40);
            const auto render = [](const std::set<std::uint64_t>& sequences) {
                std::string value;
                for (const std::uint64_t sequence : sequences) {
                    value += (value.empty() ? "" : ",") + std::to_string(sequence);
                }
                return value;
            };
            expect(localLive == std::set<std::uint64_t>{41, 42, 43} && remoteLive == std::set<std::uint64_t>{41, 43},
                   "local_trusted sees 41/42/43 while default_remote accepts sparse live 41/43 with no fabricated 42 (local=" +
                       render(localLive) + ", remote=" + render(remoteLive) + ")");

            explicitReplayBaseline = remoteObservations.messages.size();
            expect(remote->receive(command("sparse-explicit-replay", frontend::ReplayAfter{frontend::SequenceNumber{40}})).accepted(),
                   "explicit events.replay accepts the global cursor before the sparse interval");
            waitUntil(
                "explicit sparse replay completes",
                [this]() {
                    return hasSuccessfulResponse(remoteObservations, "sparse-explicit-replay") &&
                           firstSync(remoteObservations, explicitReplayBaseline).has_value();
                },
                [this]() {
                    const std::size_t syncIndex = *firstSync(remoteObservations, explicitReplayBaseline);
                    const auto* complete = std::get_if<frontend::SyncComplete>(&remoteObservations.messages[syncIndex]);
                    expect(complete && complete->sequence == frontend::SequenceNumber{43} &&
                               messageSequences(remoteObservations, explicitReplayBaseline, syncIndex) == std::set<std::uint64_t>{41, 43} &&
                               hasBatchRange(remoteObservations,
                                             explicitReplayBaseline,
                                             syncIndex,
                                             frontend::SequenceNumber{41},
                                             frontend::SequenceNumber{43}),
                           "explicit replay preserves sparse 41/43 and sync.complete advances the global cursor to 43");
                    startVisibleReconnect();
                });
        }

        void startVisibleReconnect() {
            reconnect.emplace(service->openConnection(remotePeer(), callbacksFor(reconnectObservations)));
            expect(reconnect->receive(remoteHello(frontend::SequenceNumber{41})).accepted(),
                   "a legacy default_remote reconnect resumes from visible global sequence 41");
            waitUntil(
                "legacy sparse reconnect completes",
                [this]() {
                    return firstSync(reconnectObservations).has_value();
                },
                [this]() {
                    const std::size_t syncIndex = *firstSync(reconnectObservations);
                    const auto* complete = std::get_if<frontend::SyncComplete>(&reconnectObservations.messages[syncIndex]);
                    expect(welcome(reconnectObservations) && welcome(reconnectObservations)->syncMode == frontend::SyncMode::Replay &&
                               complete && complete->sequence == frontend::SequenceNumber{43} &&
                               messageSequences(reconnectObservations, 0, syncIndex) == std::set<std::uint64_t>{43},
                           "legacy reconnect after 41 replays visible 43 at the same information ceiling and exposes no hidden marker");
                    beginHiddenInterval();
                });
        }

        void beginHiddenInterval() {
            reconnect.reset();
            remote.reset();
            local.reset();
            service->close("visible sparse interval complete");
            service.reset();
            afterTicks(8, [this]() {
                service = std::make_unique<frontend::FrontendService>(*backendCore, serviceOptions(frontend::SequenceNumber{50}, 2));
                injectVisibleFirst();
                waitUntil(
                    "hidden-suffix visible anchor reaches global 51",
                    [this]() {
                        return service->currentSequence() == frontend::SequenceNumber{51};
                    },
                    [this]() {
                        injectHidden("aGlkZGVuLXN1ZmZpeC0x");
                        waitUntil(
                            "first hidden suffix occurrence reaches global 52",
                            [this]() {
                                return service->currentSequence() == frontend::SequenceNumber{52};
                            },
                            [this]() {
                                injectHidden("aGlkZGVuLXN1ZmZpeC0y");
                                waitUntil(
                                    "second hidden suffix occurrence reaches global 53",
                                    [this]() {
                                        return service->currentSequence() == frontend::SequenceNumber{53};
                                    },
                                    [this]() {
                                        startHiddenSuffixReconnect();
                                    });
                            });
                    });
            });
        }

        void startHiddenSuffixReconnect() {
            suffix.emplace(service->openConnection(remotePeer(), callbacksFor(suffixObservations)));
            expect(suffix->receive(remoteHello(frontend::SequenceNumber{51})).accepted(),
                   "default_remote reconnect accepts a global cursor before a fully hidden suffix");
            waitUntil(
                "hidden suffix advances sync and later session event remains usable",
                [this]() {
                    const auto sync = firstSync(suffixObservations);
                    return sync.has_value() && service->currentSequence() >= frontend::SequenceNumber{54} &&
                           messageSequences(suffixObservations, *sync + 1, suffixObservations.messages.size()).contains(54);
                },
                [this]() {
                    const std::size_t syncIndex = *firstSync(suffixObservations);
                    const auto* complete = std::get_if<frontend::SyncComplete>(&suffixObservations.messages[syncIndex]);
                    expect(welcome(suffixObservations) && welcome(suffixObservations)->syncMode == frontend::SyncMode::Replay && complete &&
                               complete->sequence == frontend::SequenceNumber{53} &&
                               messageSequences(suffixObservations, 0, syncIndex).empty() && suffix->isOpen(),
                           "a nonempty hidden suffix emits no batch, sync.complete advances to 53, and visible 54 is accepted normally");
                    startSnapshotFallback();
                });
        }

        void startSnapshotFallback() {
            fallback.emplace(service->openConnection(remotePeer(), callbacksFor(fallbackObservations)));
            expect(fallback->receive(remoteHello(frontend::SequenceNumber{50})).accepted(),
                   "a cursor below the replay floor requests synchronization without inferring visible loss");
            waitUntil(
                "sparse replay-gap snapshot fallback completes",
                [this]() {
                    return firstSync(fallbackObservations).has_value();
                },
                [this]() {
                    const std::size_t syncIndex = *firstSync(fallbackObservations);
                    const auto* complete = std::get_if<frontend::SyncComplete>(&fallbackObservations.messages[syncIndex]);
                    const frontend::Snapshot* snapshot = latestSnapshot(fallbackObservations);
                    const bool exposesHiddenType = std::any_of(
                        fallbackObservations.compactJson.begin(), fallbackObservations.compactJson.end(), [](const std::string& encoded) {
                            return encoded.find("process.updated") != std::string::npos ||
                                   encoded.find("process/outputDelta") != std::string::npos;
                        });
                    expect(welcome(fallbackObservations) && welcome(fallbackObservations)->syncMode == frontend::SyncMode::Snapshot &&
                               snapshot && complete && snapshot->sequence == frontend::SequenceNumber{54} &&
                               complete->sequence == frontend::SequenceNumber{54} && countSnapshots(fallbackObservations) == 1 &&
                               messageSequences(fallbackObservations, 0, syncIndex).empty() && !exposesHiddenType,
                           "an unavailable canonical interval uses one snapshot at cursor 54, never mixes replay, and discloses no hidden "
                           "occurrence type (welcome=" +
                               std::to_string(welcome(fallbackObservations) != nullptr) +
                               ", snapshot=" + (snapshot ? std::to_string(snapshot->sequence.value()) : std::string("none")) +
                               ", complete=" + (complete ? std::to_string(complete->sequence.value()) : std::string("none")) +
                               ", snapshots=" + std::to_string(countSnapshots(fallbackObservations)) +
                               ", pre-sync-events=" + std::to_string(messageSequences(fallbackObservations, 0, syncIndex).size()) +
                               ", hidden-type=" + std::to_string(exposesHiddenType) + ")");
                    finish();
                });
        }

        void finish() {
            if (finished || finishing) {
                return;
            }
            finishing = true;
            fallback.reset();
            suffix.reset();
            reconnect.reset();
            remote.reset();
            local.reset();
            if (service) {
                service->close("sparse-sequence runner complete");
                service.reset();
            }
            if (backendCore) {
                backendCore->stop();
            }
            afterTicks(8, [this]() {
                backendCore.reset();
                transport.reset();
                finishing = false;
                finished = true;
                waitingDescription = "complete";
                if (onFinished) {
                    onFinished();
                }
            });
        }

        tests::support::TestResult& result;
        std::function<void()> onFinished;
        std::shared_ptr<tests::codex::FakeTransportState> transport;
        std::unique_ptr<FakeBackendCore> backendCore;
        std::unique_ptr<frontend::FrontendService> service;
        std::optional<frontend::FrontendConnection> local;
        std::optional<frontend::FrontendConnection> remote;
        std::optional<frontend::FrontendConnection> reconnect;
        std::optional<frontend::FrontendConnection> suffix;
        std::optional<frontend::FrontendConnection> fallback;
        Observations localObservations;
        Observations remoteObservations;
        Observations reconnectObservations;
        Observations suffixObservations;
        Observations fallbackObservations;
        std::size_t localBaseline = 0;
        std::size_t remoteBaseline = 0;
        std::size_t explicitReplayBaseline = 0;
        bool finishing = false;
        bool finished = false;
        std::string waitingDescription = "not started";
    };

    class FrontendBurstRunner {
    public:
        explicit FrontendBurstRunner(tests::support::TestResult& result)
            : result(result) {
        }

        void start() {
            transport = std::make_shared<tests::codex::FakeTransportState>();
            tests::codex::installInitializingFake(transport, [this](const Json& message, const TransportCallbacks& callbacks) {
                handleOutgoing(message, callbacks);
            });

            backend::BackendCoreOptions backendOptions;
            backendOptions.initialThreadListLimit = 1;
            backendOptions.maxObserverQueueEntries = 2'048;
            backendOptions.maxObserverQueueBytes = 32U * 1024U * 1024U;
            backendOptions.maxEventsPerCallback = 2'048;
            backendOptions.capacity.maxRetainedThreads = 1;
            backendCore = std::make_unique<FakeBackendCore>(std::move(backendOptions), transport);

            frontend::FrontendServiceOptions serviceOptions;
            enableVerifiedTestTrust(serviceOptions);
            serviceOptions.journal = {128, 2U * 1024U * 1024U, frontend::SequenceNumber{0}};
            serviceOptions.batches = {8, 128U * 1024U};
            serviceOptions.coalescer = {64};
            serviceOptions.maxOutboundMessagesPerConnection = maxOutboundMessages;
            serviceOptions.maxOutboundBytesPerConnection = maxOutboundBytes;
            serviceOptions.maxMessagesPerDelivery = 4;
            service = std::make_unique<frontend::FrontendService>(*backendCore, std::move(serviceOptions));

            connectionA.emplace(service->openConnection(trustedPeer(), callbacksFor(observerA)));
            connectionB.emplace(service->openConnection(trustedPeer(), callbacksFor(observerB)));
            rangeLegacyConnection.emplace(service->openConnection(trustedPeer(), callbacksFor(rangeLegacyObserver)));
            const frontend::Hello expandedHello{
                std::nullopt,
                frontend::Json::object(),
                std::vector{frontend::FrontendCapability::DedicatedNotificationEvents},
            };
            expect(connectionA->receive(frontend::ClientMessage{expandedHello}).accepted() &&
                       connectionB->receive(frontend::ClientMessage{expandedHello}).accepted() &&
                       rangeLegacyConnection->receive(hello()).accepted(),
                   "the two equivalent expanded sessions and dedicated legacy projection session accept hello");

            backendCore->start();
            waitUntil(
                "frontend burst backend reaches Ready and completes bounded hydration",
                [this]() {
                    const backend::Snapshot snapshot = backendCore->snapshot();
                    return backendCore->isReady() && snapshot.threadList.pagesLoaded == 1 && connectionA->helloComplete() &&
                           connectionB->helloComplete() && rangeLegacyConnection->helloComplete();
                },
                [this]() {
                    exerciseCanonicalOccurrenceSequence();
                });
        }

        bool isFinished() const noexcept {
            return finished;
        }

        const std::string& waitingStage() const noexcept {
            return waitingDescription;
        }

        std::string terminalProgress() const {
            if (!backendCore) {
                return "runner finished";
            }
            const std::string backendText = terminalBackendText();
            const std::string frontendTextA = latestFrontendText(observerA);
            const std::string frontendTextB = latestFrontendText(observerB);
            return "backend=" + std::to_string(backendText.size()) + "/" + std::to_string(expectedText.size()) +
                   ", frontendA=" + std::to_string(frontendTextA.size()) + ", frontendB=" + std::to_string(frontendTextB.size()) +
                   ", user=" +
                   std::to_string(hasCompletedItemUpdate(
                       observerA, userMessageItemId, "user_message", userMessageStartedAtMs, userMessageCompletedAtMs)) +
                   "/" +
                   std::to_string(hasCompletedItemUpdate(
                       observerB, userMessageItemId, "user_message", userMessageStartedAtMs, userMessageCompletedAtMs)) +
                   ", unknown=" +
                   std::to_string(hasCompletedItemUpdate(
                       observerA, unknownItemId, unknownItemType, unknownItemStartedAtMs, unknownItemCompletedAtMs)) +
                   "/" +
                   std::to_string(hasCompletedItemUpdate(
                       observerB, unknownItemId, unknownItemType, unknownItemStartedAtMs, unknownItemCompletedAtMs)) +
                   ", legacy=" + std::to_string(hasMetadataOnlyLegacyItemUpdates(observerA)) + "/" +
                   std::to_string(hasMetadataOnlyLegacyItemUpdates(observerB));
        }

    private:
        static void defer(std::function<void()> callback) {
            core::EventReceiver::atNextTick(std::move(callback));
        }

        void expect(bool condition, const std::string& message) {
            result.expectTrue(condition, message);
        }

        void
        waitUntil(std::string description, std::function<bool()> predicate, std::function<void()> next, std::size_t remaining = 8'000) {
            waitingDescription = description;
            defer([this,
                   description = std::move(description),
                   predicate = std::move(predicate),
                   next = std::move(next),
                   remaining]() mutable {
                if (finished) {
                    return;
                }
                if (predicate()) {
                    waitingDescription = "advancing after: " + description;
                    next();
                    return;
                }
                if (remaining == 0) {
                    expect(false, description);
                    finish();
                    return;
                }
                waitUntil(std::move(description), std::move(predicate), std::move(next), remaining - 1);
            });
        }

        void afterTicks(std::size_t count, std::function<void()> next) {
            if (count == 0) {
                next();
                return;
            }
            defer([this, count, next = std::move(next)]() mutable {
                afterTicks(count - 1, std::move(next));
            });
        }

        void handleOutgoing(const Json& message, const TransportCallbacks& callbacks) {
            const auto method = message.find("method");
            const auto id = message.find("id");
            if (method == message.end() || !method->is_string() || id == message.end()) {
                return;
            }
            if (*method == "thread/list") {
                const Json params = message.value("params", Json::object());
                const bool frontendRequest = params.value("cursor", "") == wrapperCursor;
                tests::codex::inject(
                    callbacks,
                    Json{{"id", *id},
                         {"result",
                          {{"data", frontendRequest ? Json::array({tests::codex::threadValue(wrapperThreadId)}) : Json::array()},
                           {"nextCursor", nullptr},
                           {"backwardsCursor", nullptr}}}});
            } else if (*method == "thread/start" || *method == "thread/resume") {
                tests::codex::inject(callbacks, Json{{"id", *id}, {"result", tests::codex::threadOperationResult(wrapperThreadId)}});
            } else if (*method == "thread/read") {
                const std::string threadId = message.value("params", Json::object()).value("threadId", wrapperReadThreadId);
                tests::codex::inject(callbacks, Json{{"id", *id}, {"result", {{"thread", tests::codex::threadValue(threadId)}}}});
            } else if (*method == "turn/start") {
                tests::codex::inject(
                    callbacks,
                    Json{{"id", *id}, {"result", {{"turn", tests::codex::turnValue(wrapperThreadId, wrapperTurnId, "completed")}}}});
            } else if (*method == "turn/interrupt") {
                tests::codex::inject(callbacks, Json{{"id", *id}, {"result", Json::object()}});
            }
        }

        std::vector<frontend::FrontendEvent>
        familyEventsAfter(const Observations& observations, std::size_t baseline, std::initializer_list<std::string_view> families) const {
            const std::vector<frontend::FrontendEvent> received = events(observations);
            std::vector<frontend::FrontendEvent> selected;
            for (auto iterator = received.begin() + static_cast<std::ptrdiff_t>(baseline); iterator != received.end(); ++iterator) {
                if (std::find(families.begin(), families.end(), iterator->type) != families.end()) {
                    selected.push_back(*iterator);
                }
            }
            return selected;
        }

        std::vector<frontend::FrontendEvent>
        legacyNotificationEventsAfter(const Observations& observations, std::size_t baseline, std::string_view method) const {
            const std::vector<frontend::FrontendEvent> received = events(observations);
            std::vector<frontend::FrontendEvent> selected;
            for (auto iterator = received.begin() + static_cast<std::ptrdiff_t>(baseline); iterator != received.end(); ++iterator) {
                if (iterator->type == "codex.extension" && iterator->data.value("method", "") == method) {
                    selected.push_back(*iterator);
                }
            }
            return selected;
        }

        void exerciseCanonicalOccurrenceSequence() {
            rangeBeforeConfig = service->currentSequence();
            rangeExpandedBaseline = events(observerA).size();
            rangeLegacyBaseline = events(rangeLegacyObserver).size();
            transport->inject({{"method", "configWarning"},
                               {"params",
                                {{"summary", "synthetic config warning"},
                                 {"details", "safe details"},
                                 {"path", "/synthetic/config.toml"},
                                 {"range", nullptr}}}});
            waitUntil(
                "configWarning reaches expanded and legacy projections from one canonical record",
                [this]() {
                    return familyEventsAfter(observerA, rangeExpandedBaseline, {"configuration.updated", "notice.added"}).size() == 2 &&
                           legacyNotificationEventsAfter(rangeLegacyObserver, rangeLegacyBaseline, "configWarning").size() == 1;
                },
                [this]() {
                    const std::vector<frontend::FrontendEvent> expanded =
                        familyEventsAfter(observerA, rangeExpandedBaseline, {"configuration.updated", "notice.added"});
                    const std::vector<frontend::FrontendEvent> legacy =
                        legacyNotificationEventsAfter(rangeLegacyObserver, rangeLegacyBaseline, "configWarning");
                    if (expanded.size() == 2) {
                        configSequence = expanded[0].sequence;
                    }
                    expect(expanded.size() == 2 && expanded[0].type == "configuration.updated" && configSequence > rangeBeforeConfig &&
                               expanded[1].type == "notice.added" && expanded[1].sequence == configSequence && legacy.size() == 1 &&
                               legacy.front().sequence == configSequence && service->currentSequence() == configSequence,
                           "one configWarning canonical record gives every authorized representation one occurrence sequence "
                           "(before=" +
                               std::to_string(rangeBeforeConfig.value()) + ", configuration=" + std::to_string(configSequence.value()) +
                               ", notice=" + std::to_string(expanded[1].sequence.value()) +
                               ", legacy=" + std::to_string(legacy.front().sequence.value()) +
                               ", current=" + std::to_string(service->currentSequence().value()) + ")");
                    replayAfterOccurrence();
                });
        }

        void replayAfterOccurrence() {
            rangeExpandedBaseline = events(observerA).size();
            expect(connectionA->receive(command("occurrence-after", frontend::ReplayAfter{configSequence})).accepted(),
                   "events.replay accepts the canonical multi-family occurrence sequence");
            waitUntil(
                "replay after the occurrence suppresses every family from that record",
                [this]() {
                    return hasSuccessfulResponse(observerA, "occurrence-after");
                },
                [this]() {
                    expect(familyEventsAfter(observerA, rangeExpandedBaseline, {"configuration.updated", "notice.added"}).empty(),
                           "an occurrence cursor cannot partially replay one canonical record");
                    replayBeforeOccurrence();
                });
        }

        void replayBeforeOccurrence() {
            rangeExpandedBaseline = events(observerA).size();
            expect(connectionA->receive(command("occurrence-before", frontend::ReplayAfter{rangeBeforeConfig})).accepted(),
                   "events.replay accepts the sequence immediately before a canonical multi-family occurrence");
            waitUntil(
                "replay before the occurrence emits both dedicated families atomically",
                [this]() {
                    return hasSuccessfulResponse(observerA, "occurrence-before") &&
                           familyEventsAfter(observerA, rangeExpandedBaseline, {"configuration.updated", "notice.added"}).size() == 2;
                },
                [this]() {
                    const std::vector<frontend::FrontendEvent> replayed =
                        familyEventsAfter(observerA, rangeExpandedBaseline, {"configuration.updated", "notice.added"});
                    expect(replayed.size() == 2 && replayed[0].sequence == configSequence && replayed[1].sequence == configSequence,
                           "replay before the occurrence preserves both same-sequence families without duplicates");
                    emitGuardianOccurrence();
                });
        }

        void emitGuardianOccurrence() {
            rangeExpandedBaseline = events(observerA).size();
            rangeLegacyBaseline = events(rangeLegacyObserver).size();
            transport->inject({{"method", "guardianWarning"},
                               {"params", {{"message", "Synthetic guardian warning."}, {"threadId", "synthetic-thread"}}}});
            waitUntil(
                "guardianWarning reaches expanded and legacy projections from the next canonical record",
                [this]() {
                    return familyEventsAfter(observerA, rangeExpandedBaseline, {"reviews.updated", "notice.added"}).size() == 2 &&
                           legacyNotificationEventsAfter(rangeLegacyObserver, rangeLegacyBaseline, "guardianWarning").size() == 1;
                },
                [this]() {
                    const std::vector<frontend::FrontendEvent> expanded =
                        familyEventsAfter(observerA, rangeExpandedBaseline, {"reviews.updated", "notice.added"});
                    const std::vector<frontend::FrontendEvent> legacy =
                        legacyNotificationEventsAfter(rangeLegacyObserver, rangeLegacyBaseline, "guardianWarning");
                    expect(expanded.size() == 2 && expanded[0].type == "reviews.updated" &&
                               expanded[0].sequence == frontend::SequenceNumber{configSequence.value() + 1} &&
                               expanded[1].type == "notice.added" && expanded[1].sequence == expanded[0].sequence && legacy.size() == 1 &&
                               legacy.front().sequence == expanded[0].sequence && service->currentSequence() == expanded[0].sequence,
                           "the next guardianWarning occurrence advances once and shares that sequence across projections");
                    rangeLegacyConnection.reset();
                    exerciseExactWrapperProjection();
                });
        }

        void exerciseExactWrapperProjection() {
            expect(connectionA->receive(command(wrapperAcquireRequestId, frontend::ControllerAcquire{})).accepted(),
                   "wrapper regression session acquires the BackendCore controller");
            waitUntil(
                "wrapper regression controller acquisition completes",
                [this]() {
                    return hasSuccessfulResponse(observerA, wrapperAcquireRequestId);
                },
                [this]() {
                    frontend::ThreadList list;
                    list.cursor = wrapperCursor;

                    frontend::ThreadResume resume;
                    resume.threadId = wrapperThreadId;

                    frontend::TurnStart turnStart;
                    turnStart.threadId = wrapperThreadId;
                    turnStart.input = {frontend::TextInput{"Hello from the v1 wrapper regression"}};

                    expect(
                        connectionA->receive(command(wrapperThreadStartRequestId, frontend::ThreadStart{})).accepted() &&
                            connectionA->receive(command(wrapperThreadResumeRequestId, std::move(resume))).accepted() &&
                            connectionA->receive(command(wrapperThreadListRequestId, std::move(list))).accepted() &&
                            connectionA->receive(command(wrapperTurnStartRequestId, std::move(turnStart))).accepted() &&
                            connectionA
                                ->receive(command(wrapperTurnInterruptRequestId, frontend::TurnInterrupt{wrapperThreadId, wrapperTurnId}))
                                .accepted(),
                        "the five non-read Frontend Protocol v1 provider commands remain accepted");

                    waitUntil(
                        "the five non-read exact provider response wrappers preserve the v1 result contract",
                        [this]() {
                            return response(observerA, wrapperThreadStartRequestId) != nullptr &&
                                   response(observerA, wrapperThreadResumeRequestId) != nullptr &&
                                   response(observerA, wrapperThreadListRequestId) != nullptr &&
                                   response(observerA, wrapperTurnStartRequestId) != nullptr &&
                                   response(observerA, wrapperTurnInterruptRequestId) != nullptr;
                        },
                        [this]() {
                            verifyExactWrapperProjectionBeforeRead();
                            expect(
                                connectionA->receive(command(wrapperThreadReadRequestId, frontend::ThreadRead{wrapperReadThreadId, true}))
                                    .accepted(),
                                "thread.read remains accepted after the other exact-wrapper responses are delivered");
                            waitUntil(
                                "thread.read exact response wrapper preserves the v1 result contract",
                                [this]() {
                                    return response(observerA, wrapperThreadReadRequestId) != nullptr;
                                },
                                [this]() {
                                    verifyExactThreadReadProjection();
                                    transport->inject({{"method", "thread/deleted"}, {"params", {{"threadId", wrapperReadThreadId}}}});
                                    waitUntil(
                                        "the exact-wrapper fixture releases its retained thread before the capacity scenario",
                                        [this]() {
                                            return backendCore->snapshot().threads.empty();
                                        },
                                        [this]() {
                                            afterTicks(8, [this]() {
                                                emitExtensions();
                                            });
                                        });
                                });
                        });
                });
        }

        void verifyExactWrapperProjectionBeforeRead() {
            const auto exactThreadResult = [this](const char* requestId, const char* expectedThreadId, bool fullyLoaded) {
                const frontend::Response* value = response(observerA, requestId);
                return value != nullptr && value->ok && value->result.has_value() && value->result->size() == 1 &&
                       value->result->contains("thread") && (*value->result)["thread"].is_object() &&
                       (*value->result)["thread"].value("id", "") == expectedThreadId &&
                       (*value->result)["thread"].value("fullyLoaded", !fullyLoaded) == fullyLoaded;
            };
            expect(exactThreadResult(wrapperThreadStartRequestId, wrapperThreadId, false) &&
                       exactThreadResult(wrapperThreadResumeRequestId, wrapperThreadId, false),
                   "thread start/resume exact result wrappers preserve the v1 envelope and summary-load semantics");

            const frontend::Response* list = response(observerA, wrapperThreadListRequestId);
            expect(list != nullptr && list->ok && list->result.has_value() && list->result->size() == 1 &&
                       list->result->contains("threads") && (*list->result)["threads"].is_array() &&
                       (*list->result)["threads"].size() == 1 && (*list->result)["threads"][0].value("id", "") == wrapperThreadId,
                   "thread list exact result wrapper preserves the unchanged v1 page envelope");

            const frontend::Response* turn = response(observerA, wrapperTurnStartRequestId);
            expect(turn != nullptr && turn->ok && turn->result.has_value() && turn->result->size() == 1 && turn->result->contains("turn") &&
                       (*turn->result)["turn"].is_object() && (*turn->result)["turn"].value("id", "") == wrapperTurnId,
                   "turn start exact result wrapper projects only the unchanged v1 turn envelope");

            const frontend::Response* interrupt = response(observerA, wrapperTurnInterruptRequestId);
            expect(interrupt != nullptr && interrupt->ok && interrupt->result.has_value() && interrupt->result->empty(),
                   "turn interrupt Unit result remains the unchanged empty v1 object");
        }

        void verifyExactThreadReadProjection() {
            const frontend::Response* value = response(observerA, wrapperThreadReadRequestId);
            expect(value != nullptr && value->ok && value->result.has_value() && value->result->size() == 1 &&
                       value->result->contains("thread") && (*value->result)["thread"].is_object() &&
                       (*value->result)["thread"].value("id", "") == wrapperReadThreadId &&
                       (*value->result)["thread"].value("fullyLoaded", false),
                   "thread read exact result wrapper preserves the v1 envelope and full-load semantics");
        }

        void emitBurst() {
            baselineMessagesA = observerA.messages.size();
            baselineMessagesB = observerB.messages.size();
            baselineEventsA = events(observerA).size();
            baselineEventsB = events(observerB).size();
            baselineSequence = service->currentSequence();

            transport->inject(
                {{"method", "turn/started"}, {"params", {{"threadId", threadId}, {"turn", tests::codex::turnValue(threadId, turnId)}}}});
            transport->inject(
                {{"method", "item/started"},
                 {"params", {{"threadId", threadId}, {"turnId", turnId}, {"item", agentItemValue(itemId)}, {"startedAtMs", 10}}}});

            expectedText.clear();
            expectedText.reserve(deltaCount);
            for (std::size_t index = 0; index < deltaCount; ++index) {
                const std::string delta(1, static_cast<char>('a' + static_cast<int>(index % 26)));
                expectedText += delta;
                transport->inject({{"method", "item/agentMessage/delta"},
                                   {"params", {{"threadId", threadId}, {"turnId", turnId}, {"itemId", itemId}, {"delta", delta}}}});
            }
            transport->inject(
                {{"method", "item/completed"},
                 {"params",
                  {{"threadId", threadId}, {"turnId", turnId}, {"item", agentItemValue(itemId, expectedText)}, {"completedAtMs", 20}}}});
            transport->inject(
                {{"method", "item/started"},
                 {"params",
                  {{"threadId", threadId},
                   {"turnId", turnId},
                   {"item", {{"id", userMessageItemId}, {"type", "userMessage"}, {"clientId", nullptr}, {"content", userMessageContent}}},
                   {"startedAtMs", userMessageStartedAtMs}}}});
            transport->inject(
                {{"method", "item/completed"},
                 {"params",
                  {{"threadId", threadId},
                   {"turnId", turnId},
                   {"item", {{"id", userMessageItemId}, {"type", "userMessage"}, {"clientId", nullptr}, {"content", userMessageContent}}},
                   {"completedAtMs", userMessageCompletedAtMs}}}});
            transport->inject({{"method", "item/started"},
                               {"params",
                                {{"threadId", threadId},
                                 {"turnId", turnId},
                                 {"item", {{"id", unknownItemId}, {"type", unknownItemType}, {"futureField", Json{{"value", 7}}}}},
                                 {"startedAtMs", unknownItemStartedAtMs}}}});
            transport->inject({{"method", "item/completed"},
                               {"params",
                                {{"threadId", threadId},
                                 {"turnId", turnId},
                                 {"item", {{"id", unknownItemId}, {"type", unknownItemType}, {"futureField", Json{{"value", 7}}}}},
                                 {"completedAtMs", unknownItemCompletedAtMs}}}});
            std::int64_t legacyCompletedAtMs = 70;
            for (const LegacyMetadataItemFixture& fixture : legacyMetadataItems) {
                transport->inject(
                    {{"method", "item/completed"},
                     {"params",
                      {{"threadId", threadId}, {"turnId", turnId}, {"item", fixture.item}, {"completedAtMs", legacyCompletedAtMs++}}}});
            }
            transport->inject({{"method", "turn/completed"},
                               {"params", {{"threadId", threadId}, {"turn", tests::codex::turnValue(threadId, turnId, "completed")}}}});

            waitUntil(
                "1,000 typed deltas reach exact terminal frontend state",
                [this]() {
                    return terminalBackendText() == expectedText && latestFrontendText(observerA) == expectedText &&
                           latestFrontendText(observerB) == expectedText &&
                           hasCompletedItemUpdate(
                               observerA, userMessageItemId, "user_message", userMessageStartedAtMs, userMessageCompletedAtMs) &&
                           hasCompletedItemUpdate(
                               observerA, unknownItemId, unknownItemType, unknownItemStartedAtMs, unknownItemCompletedAtMs) &&
                           hasCompletedItemUpdate(
                               observerB, userMessageItemId, "user_message", userMessageStartedAtMs, userMessageCompletedAtMs) &&
                           hasCompletedItemUpdate(
                               observerB, unknownItemId, unknownItemType, unknownItemStartedAtMs, unknownItemCompletedAtMs) &&
                           hasMetadataOnlyLegacyItemUpdates(observerA) && hasMetadataOnlyLegacyItemUpdates(observerB) &&
                           hasTerminalTurnAfter(observerA, baselineEventsA) && hasTerminalTurnAfter(observerB, baselineEventsB);
                },
                [this]() {
                    verifyBurst();
                });
        }

        void emitExtensions() {
            transport->inject(
                {{"method", "future/safe-extension"},
                 {"params",
                  {{"safe", "visible"},
                   {"accessToken", extensionAccessToken},
                   {"Client_Secret", extensionAccessToken},
                   {"nested", {{"secret", true}, {"text", extensionSecretAnswer}, {"answers", Json::array({extensionSecretAnswer})}}}}}});
            oversizedExtensionSecret.assign(backend::MaxSnapshotExtensionPayloadBytes * 3, 'x');
            transport->inject({{"method", "future/oversized-extension"},
                               {"params", {{"authorization", oversizedExtensionSecret}, {"padding", oversizedExtensionSecret}}}});

            waitUntil(
                "both sanitized extension events reach the frontend",
                [this]() {
                    const std::vector<frontend::FrontendEvent> received = events(observerA);
                    const bool hasSafe = std::any_of(received.begin(), received.end(), [](const frontend::FrontendEvent& event) {
                        return event.type == "codex.extension" && event.data.value("method", "") == "future/safe-extension";
                    });
                    const bool hasOversized = std::any_of(received.begin(), received.end(), [](const frontend::FrontendEvent& event) {
                        return event.type == "codex.extension" && event.data.value("method", "") == "future/oversized-extension";
                    });
                    return hasSafe && hasOversized && backendCore->snapshot().recentExtensions.size() >= 2;
                },
                [this]() {
                    expect(connectionA->receive(command("extension-snapshot", frontend::SnapshotGet{})).accepted(),
                           "frontend requests a fresh snapshot after unknown extensions");
                    waitUntil(
                        "sanitized extension history reaches an explicit frontend snapshot",
                        [this]() {
                            const frontend::Snapshot* snapshot = latestSnapshot(observerA);
                            return snapshot && snapshot->state.contains("codexExtensions") &&
                                   snapshot->state["codexExtensions"].is_array() && snapshot->state["codexExtensions"].size() >= 2 &&
                                   hasSuccessfulResponse(observerA, "extension-snapshot");
                        },
                        [this]() {
                            verifyExtensions();
                            afterTicks(8, [this]() {
                                emitBurst();
                            });
                        });
                });
        }

        void verifyExtensions() {
            const std::vector<frontend::FrontendEvent> received = events(observerA);
            const auto safeEvent = std::find_if(received.rbegin(), received.rend(), [](const frontend::FrontendEvent& event) {
                return event.type == "codex.extension" && event.data.value("method", "") == "future/safe-extension";
            });
            const auto oversizedEvent = std::find_if(received.rbegin(), received.rend(), [](const frontend::FrontendEvent& event) {
                return event.type == "codex.extension" && event.data.value("method", "") == "future/oversized-extension";
            });
            expect(safeEvent != received.rend() && safeEvent->data.at("params").at("safe") == "visible" &&
                       safeEvent->data.value("sensitiveFieldsRedacted", false),
                   "codex.extension preserves safe unknown fields and explicitly reports recursive redaction");
            expect(oversizedEvent != received.rend() && oversizedEvent->data.at("params").value("omitted", false) &&
                       oversizedEvent->data.contains("truncation"),
                   "an oversized unknown event remains observable as one explicit bounded codex.extension record");

            const frontend::Snapshot* snapshot = latestSnapshot(observerA);
            expect(snapshot && snapshot->state["codexExtensions"].back().value("method", "") == "future/oversized-extension" &&
                       snapshot->state["codexExtensions"].back()["params"].value("omitted", false),
                   "snapshot fallback includes the reducer-retained sanitized extension history");

            const bool secretLeaked =
                std::any_of(observerA.compactJson.begin(), observerA.compactJson.end(), [this](const std::string& compact) {
                    return compact.find(extensionAccessToken) != std::string::npos ||
                           compact.find(extensionSecretAnswer) != std::string::npos ||
                           compact.find(oversizedExtensionSecret) != std::string::npos;
                });
            const bool extensionMessagesBounded =
                std::all_of(observerA.messages.begin(), observerA.messages.end(), [](const frontend::ServerMessage& message) {
                    if (const auto* batch = std::get_if<frontend::EventBatch>(&message)) {
                        const bool hasExtension =
                            std::any_of(batch->events.begin(), batch->events.end(), [](const frontend::FrontendEvent& event) {
                                return event.type == "codex.extension";
                            });
                        if (hasExtension) {
                            const auto encoded = frontend::Codec::serializeServer(message);
                            return encoded && encoded.value().size() <= backend::MaxSerializedCodexExtensionEventBytes;
                        }
                    }
                    return true;
                });
            expect(!secretLeaked && extensionMessagesBounded,
                   "compact extension snapshots/events contain no access tokens or secret answers and stay within the 64 KiB event bound");
        }

        std::string terminalBackendText() const {
            const backend::Snapshot snapshot = backendCore->snapshot();
            for (const backend::ThreadSnapshot& thread : snapshot.threads) {
                if (thread.id != threadId) {
                    continue;
                }
                for (const backend::TurnSnapshot& turn : thread.turns) {
                    if (turn.id != turnId || !turn.terminal) {
                        continue;
                    }
                    for (const backend::ItemSnapshot& item : turn.items) {
                        if (item.id == itemId) {
                            return item.agentText;
                        }
                    }
                }
            }
            return {};
        }

        std::string latestFrontendText(const Observations& observations) const {
            std::string latest;
            const std::vector<frontend::FrontendEvent> received = events(observations);
            for (const frontend::FrontendEvent& event : received) {
                if (event.type == "item.content.updated" && event.data.value("itemId", "") == itemId &&
                    event.data.value("channel", "") == "agentText") {
                    latest = event.data.value("content", "");
                }
            }
            return latest;
        }

        bool isCompletedItemUpdate(const frontend::FrontendEvent& event,
                                   const std::string& expectedItemId,
                                   const std::string& expectedType,
                                   std::int64_t expectedStartedAtMs,
                                   std::int64_t expectedCompletedAtMs) const {
            if (event.type != "item.updated" || event.data.value("threadId", "") != threadId || event.data.value("turnId", "") != turnId) {
                return false;
            }
            const auto item = event.data.find("item");
            return item != event.data.end() && item->is_object() && item->value("id", "") == expectedItemId &&
                   item->value("type", "") == expectedType && item->value("status", "") == "completed" &&
                   item->value("startedAtMs", std::int64_t{-1}) == expectedStartedAtMs &&
                   item->value("completedAtMs", std::int64_t{-1}) == expectedCompletedAtMs;
        }

        bool hasCompletedItemUpdate(const Observations& observations,
                                    const std::string& expectedItemId,
                                    const std::string& expectedType,
                                    std::int64_t expectedStartedAtMs,
                                    std::int64_t expectedCompletedAtMs) const {
            const std::vector<frontend::FrontendEvent> received = events(observations);
            return std::any_of(received.begin(), received.end(), [&](const frontend::FrontendEvent& event) {
                return isCompletedItemUpdate(event, expectedItemId, expectedType, expectedStartedAtMs, expectedCompletedAtMs);
            });
        }

        bool hasMetadataOnlyLegacyItemUpdates(const Observations& observations) const {
            const std::vector<frontend::FrontendEvent> received = events(observations);
            return std::all_of(legacyMetadataItems.begin(), legacyMetadataItems.end(), [&](const LegacyMetadataItemFixture& fixture) {
                return std::any_of(received.begin(), received.end(), [&](const frontend::FrontendEvent& event) {
                    if (event.type != "item.updated" || event.data.value("threadId", "") != threadId ||
                        event.data.value("turnId", "") != turnId) {
                        return false;
                    }
                    const auto item = event.data.find("item");
                    if (item == event.data.end() || !item->is_object() || item->value("id", "") != fixture.id ||
                        item->value("type", "") != fixture.type) {
                        return false;
                    }
                    const Json data = item->value("data", Json::object());
                    return data == Json{{"codexType", fixture.type}};
                });
            });
        }

        std::vector<frontend::FrontendEvent> burstEvents(const Observations& observations, std::size_t baseline) const {
            std::vector<frontend::FrontendEvent> received = events(observations);
            if (baseline >= received.size()) {
                return {};
            }
            return {received.begin() + static_cast<std::ptrdiff_t>(baseline), received.end()};
        }

        std::size_t eventCountAfter(const Observations& observations, std::size_t baseline, std::string_view type) const {
            const std::vector<frontend::FrontendEvent> received = events(observations);
            if (baseline >= received.size()) {
                return 0;
            }
            return static_cast<std::size_t>(std::count_if(
                received.begin() + static_cast<std::ptrdiff_t>(baseline), received.end(), [type](const frontend::FrontendEvent& event) {
                    return event.type == type;
                }));
        }

        bool hasTerminalTurnAfter(const Observations& observations, std::size_t baseline) const {
            const std::vector<frontend::FrontendEvent> received = events(observations);
            if (baseline >= received.size()) {
                return false;
            }
            return std::any_of(
                received.begin() + static_cast<std::ptrdiff_t>(baseline), received.end(), [this](const frontend::FrontendEvent& event) {
                    const auto turn = event.data.find("turn");
                    return event.type == "turn.upserted" && turn != event.data.end() && turn->is_object() &&
                           turn->value("id", "") == turnId && turn->value("terminal", false);
                });
        }

        std::size_t resolvedExtensionCountAfter(const Observations& observations, std::size_t baseline) const {
            const std::vector<frontend::FrontendEvent> received = events(observations);
            if (baseline >= received.size()) {
                return 0;
            }
            return static_cast<std::size_t>(std::count_if(
                received.begin() + static_cast<std::ptrdiff_t>(baseline), received.end(), [](const frontend::FrontendEvent& event) {
                    return event.type == "codex.extension" && event.data.value("method", "") == "serverRequest/resolved";
                }));
        }

        void verifyGenericPendingProjection(const Observations& observations, std::size_t baseline) {
            struct ExpectedProjection {
                std::string_view backendType;
                std::optional<std::string_view> legacyMethod;
            };
            static constexpr std::array Expected{
                ExpectedProjection{"apply_patch_approval", "applyPatchApproval"},
                ExpectedProjection{"exec_command_approval", "execCommandApproval"},
                ExpectedProjection{"permissions_approval", "item/permissions/requestApproval"},
                ExpectedProjection{"attestation", std::nullopt},
                ExpectedProjection{"dynamic_tool_call", std::nullopt},
                ExpectedProjection{"mcp_elicitation", std::nullopt},
            };

            const backend::Snapshot snapshot = backendCore->snapshot();
            const std::vector<frontend::FrontendEvent> received = events(observations);
            const auto begin = received.begin() + static_cast<std::ptrdiff_t>(std::min(baseline, received.size()));
            bool allGeneric = true;
            for (const ExpectedProjection& expected : Expected) {
                const auto pending = std::find_if(snapshot.pendingRequests.begin(), snapshot.pendingRequests.end(), [&](const auto& value) {
                    return value.type == expected.backendType;
                });
                if (pending == snapshot.pendingRequests.end()) {
                    allGeneric = false;
                    continue;
                }
                const std::string id = std::to_string(pending->id.value());
                const auto event = std::find_if(begin, received.end(), [&](const frontend::FrontendEvent& value) {
                    return value.type == "request.pending" && value.data.contains("request") && value.data["request"].value("id", "") == id;
                });
                if (event == received.end()) {
                    allGeneric = false;
                    continue;
                }
                const Json request = event->data["request"];
                const Json details = request.value("details", Json::object());
                Json expectedLegacyDetails = pending->details;
                expectedLegacyDetails.erase("summary");
                const bool shapeMatches = expected.legacyMethod ? details.value("method", "") == *expected.legacyMethod &&
                                                                      details == expectedLegacyDetails && !details.contains("summary")
                                                                : details.empty();
                allGeneric = allGeneric && request.value("type", "") == "unknown" && shapeMatches &&
                             request.dump().find(expected.backendType) == std::string::npos;
            }
            expect(allGeneric,
                   "six meaningful BackendCore request kinds retain the bounded redacted generic unknown-request shape in Frontend "
                   "Protocol v1");
        }

        void verifyBurst() {
            const std::vector<frontend::FrontendEvent> receivedA = burstEvents(observerA, baselineEventsA);
            const std::vector<frontend::FrontendEvent> receivedB = burstEvents(observerB, baselineEventsB);
            const std::size_t frontendMessagesA = observerA.messages.size() - baselineMessagesA;
            const std::size_t frontendMessagesB = observerB.messages.size() - baselineMessagesB;

            expect(connectionA->isOpen() && connectionB->isOpen(), "both protocol sessions remain open after 1,000 fine-grained deltas");
            expect(connectionA->queuedMessages() <= maxOutboundMessages && connectionB->queuedMessages() <= maxOutboundMessages &&
                       connectionA->queuedBytes() <= maxOutboundBytes && connectionB->queuedBytes() <= maxOutboundBytes,
                   "per-connection queues remain within explicit message and byte bounds during the delta burst");
            expect(frontendMessagesA < deltaCount / 10 && frontendMessagesB < deltaCount / 10 && receivedA.size() < deltaCount / 10 &&
                       receivedB.size() < deltaCount / 10,
                   "1,000 App Server deltas produce substantially fewer than 1,000 frontend messages and updates");
            expect(receivedA == receivedB && !receivedA.empty(),
                   "controller-independent protocol sessions receive identical normalized burst updates");
            expect(std::is_sorted(receivedA.begin(),
                                  receivedA.end(),
                                  [](const frontend::FrontendEvent& left, const frontend::FrontendEvent& right) {
                                      return left.sequence < right.sequence;
                                  }) &&
                       service->currentSequence() > baselineSequence,
                   "coalesced burst updates retain strict frontend sequence order");
            std::optional<std::size_t> finalContentIndex;
            std::optional<std::size_t> terminalTurnIndex;
            for (std::size_t index = 0; index < receivedA.size(); ++index) {
                const frontend::FrontendEvent& event = receivedA[index];
                if (event.type == "item.content.updated" && event.data.value("itemId", "") == itemId &&
                    event.data.value("content", "") == expectedText) {
                    finalContentIndex = index;
                }
                const auto turn = event.data.find("turn");
                if (event.type == "turn.upserted" && turn != event.data.end() && turn->is_object() && turn->value("id", "") == turnId &&
                    turn->value("terminal", false)) {
                    terminalTurnIndex = index;
                }
            }
            expect(finalContentIndex.has_value() && terminalTurnIndex.has_value() && *finalContentIndex < *terminalTurnIndex,
                   "service emits final item.content.updated before terminal turn.updated when turn.started was already dirty");
            expect(latestFrontendText(observerA) == expectedText && latestFrontendText(observerB) == expectedText,
                   "coalescing preserves exact final text for every protocol session");

            const auto countCompletedItemUpdates = [&](const std::string& expectedItemId,
                                                       const std::string& expectedType,
                                                       std::int64_t expectedStartedAtMs,
                                                       std::int64_t expectedCompletedAtMs) {
                return std::count_if(receivedA.begin(), receivedA.end(), [&](const frontend::FrontendEvent& event) {
                    return isCompletedItemUpdate(event, expectedItemId, expectedType, expectedStartedAtMs, expectedCompletedAtMs);
                });
            };
            expect(countCompletedItemUpdates(userMessageItemId, "user_message", userMessageStartedAtMs, userMessageCompletedAtMs) == 1,
                   "userMessage start/completion coalesce into one canonical completed frontend item update");
            expect(countCompletedItemUpdates(unknownItemId, unknownItemType, unknownItemStartedAtMs, unknownItemCompletedAtMs) == 1,
                   "an unknown item with common metadata coalesces into one canonical completed frontend item update");
            const auto userMessageUpdate = std::find_if(receivedA.begin(), receivedA.end(), [&](const frontend::FrontendEvent& event) {
                return isCompletedItemUpdate(event, userMessageItemId, "user_message", userMessageStartedAtMs, userMessageCompletedAtMs);
            });
            const Json userMessageItem =
                userMessageUpdate == receivedA.end() ? Json::object() : userMessageUpdate->data.value("item", Json::object());
            const Json userMessageData = userMessageItem.is_object() ? userMessageItem.value("data", Json::object()) : Json::object();
            const Json retainedUserMessageContent = userMessageData.value("content", Json::object());
            bool userMessagePrefixPreserved = retainedUserMessageContent.is_array() && !retainedUserMessageContent.empty() &&
                                              retainedUserMessageContent.size() < userMessageContent.size();
            if (retainedUserMessageContent.is_array()) {
                for (std::size_t index = 0; index < retainedUserMessageContent.size(); ++index) {
                    userMessagePrefixPreserved = userMessagePrefixPreserved &&
                                                 retainedUserMessageContent[index] == userMessageContent[index] &&
                                                 retainedUserMessageContent[index].dump() == userMessageContent[index].dump();
                }
            }
            const bool userMessageDataPreserved =
                userMessageData.is_object() && userMessageData.contains("clientId") && userMessageData.at("clientId").is_null() &&
                userMessageContent.dump().size() > backend::MaxSerializedUserMessageDataBytes && userMessagePrefixPreserved &&
                userMessageData.value("contentTruncated", false) &&
                userMessageData.value("originalContentBytes", std::uint64_t{0}) == userMessageContent.dump().size() &&
                userMessageData.value("retainedContentBytes", std::uint64_t{0}) == retainedUserMessageContent.dump().size() &&
                userMessageData.value("originalContentItems", std::uint64_t{0}) == userMessageContent.size() &&
                userMessageData.value("retainedContentItems", std::uint64_t{0}) == retainedUserMessageContent.size() &&
                userMessageData.dump().size() <= backend::MaxSerializedUserMessageDataBytes &&
                !userMessageItem.value("contentTruncated", true) && userMessageItem.value("droppedContentBytes", std::uint64_t{1}) == 0;
            expect(userMessageDataPreserved,
                   "Decoder through FrontendService preserves a bounded array prefix and independent user-message truncation metadata");
            const auto unknownUpdate = std::find_if(receivedA.begin(), receivedA.end(), [&](const frontend::FrontendEvent& event) {
                return isCompletedItemUpdate(event, unknownItemId, unknownItemType, unknownItemStartedAtMs, unknownItemCompletedAtMs);
            });
            const Json unknownData =
                unknownUpdate == receivedA.end() ? Json::object() : unknownUpdate->data.at("item").value("data", Json::object());
            expect(unknownData.is_object() && unknownData.value("codexType", "") == unknownItemType,
                   "the canonical frontend unknown item preserves its future discriminator");
            expect(hasMetadataOnlyLegacyItemUpdates(observerA) && hasMetadataOnlyLegacyItemUpdates(observerB),
                   "all ten newly rich backend ThreadItem projections retain their metadata-only Frontend Protocol v1 payloads");

            const bool hasItemLifecycleExtension =
                std::any_of(receivedA.begin(), receivedA.end(), [](const frontend::FrontendEvent& event) {
                    if (event.type != "codex.extension") {
                        return false;
                    }
                    const std::string method = event.data.value("method", "");
                    const std::string decodingError = event.data.value("decodingError", "");
                    return method == "item/started" || method == "item/completed" ||
                           decodingError.find("item event omitted threadId or turnId") != std::string::npos;
                });
            expect(!hasItemLifecycleExtension,
                   "valid userMessage and unknown item lifecycle events never become location-error codex.extension events");

            verifyCapacityEvictionSnapshot();
        }

        void verifyCapacityEvictionSnapshot() {
            const std::size_t snapshotBaselineA = countSnapshots(observerA);
            const std::size_t snapshotBaselineB = countSnapshots(observerB);
            const std::string replacementThreadId = "capacity-replacement";
            transport->inject({{"method", "thread/started"}, {"params", {{"thread", tests::codex::threadValue(replacementThreadId)}}}});
            waitUntil(
                "retention eviction produces a frontend snapshot barrier",
                [this, snapshotBaselineA, snapshotBaselineB, replacementThreadId]() {
                    const backend::Snapshot snapshot = backendCore->snapshot();
                    return snapshot.threads.size() == 1 && snapshot.threads.front().id == replacementThreadId &&
                           countSnapshots(observerA) > snapshotBaselineA && countSnapshots(observerB) > snapshotBaselineB;
                },
                [this, replacementThreadId]() {
                    const frontend::Snapshot* snapshotA = latestSnapshot(observerA);
                    const frontend::Snapshot* snapshotB = latestSnapshot(observerB);
                    const auto reflectsReplacement = [&replacementThreadId](const frontend::Snapshot* snapshot) {
                        return snapshot != nullptr && snapshot->state.contains("threads") && snapshot->state["threads"].is_array() &&
                               snapshot->state["threads"].size() == 1 &&
                               snapshot->state["threads"].front().value("id", "") == replacementThreadId;
                    };
                    expect(reflectsReplacement(snapshotA) && reflectsReplacement(snapshotB),
                           "capacity-driven eviction invalidates replay and synchronizes every v1 client to canonical retained state");
                    verifyPendingCompatibility();
                });
        }

        void verifyPendingCompatibility() {
            pendingBaselineA = events(observerA).size();
            pendingBaselineB = events(observerB).size();

            transport->inject({{"method", "applyPatchApproval"},
                               {"id", "v1-apply-request"},
                               {"params",
                                {{"callId", "v1-apply-call"},
                                 {"conversationId", "v1-apply-thread"},
                                 {"fileChanges", Json::object()},
                                 {"grantRoot", nullptr},
                                 {"reason", "v1 apply reason"}}}});
            transport->inject({{"method", "execCommandApproval"},
                               {"id", "v1-exec-request"},
                               {"params",
                                {{"approvalId", nullptr},
                                 {"callId", "v1-exec-call"},
                                 {"command", Json::array({"echo", "v1"})},
                                 {"conversationId", "v1-exec-thread"},
                                 {"cwd", "/synthetic/v1"},
                                 {"parsedCmd", Json::array({{{"type", "unknown"}, {"cmd", "echo"}}})},
                                 {"reason", nullptr}}}});
            transport->inject({{"method", "item/permissions/requestApproval"},
                               {"id", resolvedProviderRequestId},
                               {"params",
                                {{"cwd", "/synthetic/v1"},
                                 {"environmentId", nullptr},
                                 {"itemId", "v1-permissions-item"},
                                 {"permissions", {{"fileSystem", nullptr}, {"network", {{"enabled", false}}}}},
                                 {"reason", "v1 permissions reason"},
                                 {"startedAtMs", 1},
                                 {"threadId", resolvedThreadId},
                                 {"turnId", "v1-permissions-turn"}}}});
            transport->inject({{"method", "attestation/generate"}, {"id", "v1-attestation-request"}, {"params", Json::object()}});
            transport->inject({{"method", "item/tool/call"},
                               {"id", "v1-dynamic-tool-request"},
                               {"params",
                                {{"arguments", {{"safe", true}}},
                                 {"callId", "v1-dynamic-call"},
                                 {"namespace", nullptr},
                                 {"threadId", "v1-dynamic-thread"},
                                 {"tool", "v1_tool"},
                                 {"turnId", "v1-dynamic-turn"}}}});
            transport->inject({{"method", "mcpServer/elicitation/request"},
                               {"id", "v1-mcp-request"},
                               {"params",
                                {{"_meta", Json::object()},
                                 {"message", "Synthetic v1 elicitation"},
                                 {"mode", "form"},
                                 {"requestedSchema", {{"properties", Json::object()}, {"required", Json::array()}, {"type", "object"}}},
                                 {"serverName", "v1-mcp"},
                                 {"threadId", "v1-mcp-thread"},
                                 {"turnId", nullptr}}}});

            waitUntil(
                "all six dedicated backend request kinds reach the v1 generic pending contract",
                [this]() {
                    return backendCore->snapshot().pendingRequests.size() == 6 &&
                           eventCountAfter(observerA, pendingBaselineA, "request.pending") == 6 &&
                           eventCountAfter(observerB, pendingBaselineB, "request.pending") == 6;
                },
                [this]() {
                    verifyGenericPendingProjection(observerA, pendingBaselineA);
                    verifyGenericPendingProjection(observerB, pendingBaselineB);

                    resolvedBaselineA = events(observerA).size();
                    resolvedBaselineB = events(observerB).size();
                    transport->inject({{"method", "serverRequest/resolved"},
                                       {"params", {{"requestId", resolvedProviderRequestId}, {"threadId", resolvedThreadId}}}});
                    waitUntil(
                        "matching serverRequest/resolved produces one v1 resolution without a duplicate extension",
                        [this]() {
                            return backendCore->snapshot().pendingRequests.size() == 5 &&
                                   eventCountAfter(observerA, resolvedBaselineA, "request.resolved") == 1 &&
                                   eventCountAfter(observerB, resolvedBaselineB, "request.resolved") == 1;
                        },
                        [this]() {
                            expect(eventCountAfter(observerA, resolvedBaselineA, "request.resolved") == 1 &&
                                       eventCountAfter(observerB, resolvedBaselineB, "request.resolved") == 1 &&
                                       resolvedExtensionCountAfter(observerA, resolvedBaselineA) == 0 &&
                                       resolvedExtensionCountAfter(observerB, resolvedBaselineB) == 0,
                                   "one provider resolution becomes exactly one existing v1 request.resolved event per connection");
                            verifyPendingInvalidation();
                        });
                });
        }

        void verifyPendingInvalidation() {
            pendingBaselineA = events(observerA).size();
            pendingBaselineB = events(observerB).size();
            transport->inject({{"method", "future/pending-invalidation"}, {"id", "pending-invalidation"}, {"params", Json::object()}});
            waitUntil(
                "unknown pending request reaches both frontend protocol sessions",
                [this]() {
                    return backendCore->snapshot().pendingRequests.size() == 6 &&
                           eventCountAfter(observerA, pendingBaselineA, "request.pending") == 1 &&
                           eventCountAfter(observerB, pendingBaselineB, "request.pending") == 1;
                },
                [this]() {
                    transport->callbacks.onError(Error{Error::Category::Transport, 101, "synthetic pending invalidation"});
                    waitUntil(
                        "provider invalidation resolves every retained request for both frontend sessions",
                        [this]() {
                            const backend::Snapshot snapshot = backendCore->snapshot();
                            return snapshot.provider.lifecycle == backend::ProviderLifecycle::Failed && snapshot.pendingRequests.empty() &&
                                   eventCountAfter(observerA, pendingBaselineA, "request.resolved") == 6 &&
                                   eventCountAfter(observerB, pendingBaselineB, "request.resolved") == 6;
                        },
                        [this]() {
                            expect(eventCountAfter(observerA, pendingBaselineA, "request.resolved") == 6 &&
                                       eventCountAfter(observerB, pendingBaselineB, "request.resolved") == 6,
                                   "Frontend Protocol v1 retains one request.resolved event per invalidated pending occurrence");
                            service->close("burst test complete");
                            backendCore->stop();
                            waitUntil(
                                "burst BackendCore reaches Stopped after service isolation checks",
                                [this]() {
                                    return backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Stopped;
                                },
                                [this]() {
                                    finish();
                                });
                        });
                });
        }

        void finish() {
            if (finished) {
                return;
            }
            finished = true;
            if (service) {
                service->close("frontend burst runner finished");
            }
            if (backendCore) {
                backendCore->stop();
            }
            connectionA.reset();
            connectionB.reset();
            rangeLegacyConnection.reset();
            service.reset();
            backendCore.reset();
            core::SNodeC::stop();
        }

        static constexpr std::size_t deltaCount = 1'000;
        static constexpr std::size_t maxOutboundMessages = 32;
        static constexpr std::size_t maxOutboundBytes = 1024U * 1024U;
        const std::string threadId = "thread-burst";
        const std::string turnId = "turn-burst";
        const std::string itemId = "item-burst";
        const std::string userMessageItemId = "user-message-burst";
        const Json userMessageContent = []() {
            Json content =
                Json::array({Json{{"type", "futureContent"},
                                  {"payload", Json{{"preserved", true}, {"nested", Json::array({1, Json{{"future", "value"}}})}}}}});
            for (std::size_t index = 0; index < 8; ++index) {
                content.push_back(Json{{"type", "futureLargeContent"},
                                       {"index", index},
                                       {"payload", std::string(12U * 1024U, static_cast<char>('a' + index))},
                                       {"future", Json{{"nested", index}, {"preserved", true}}}});
            }
            return content;
        }();
        static constexpr std::int64_t userMessageStartedAtMs = 30;
        static constexpr std::int64_t userMessageCompletedAtMs = 40;
        const std::string unknownItemId = "unknown-item-burst";
        const std::string unknownItemType = "futureServiceItem";
        static constexpr std::int64_t unknownItemStartedAtMs = 50;
        static constexpr std::int64_t unknownItemCompletedAtMs = 60;
        const std::vector<LegacyMetadataItemFixture> legacyMetadataItems = legacyMetadataItemFixtures();

        tests::support::TestResult& result;
        std::shared_ptr<tests::codex::FakeTransportState> transport;
        std::unique_ptr<FakeBackendCore> backendCore;
        std::unique_ptr<frontend::FrontendService> service;
        std::optional<frontend::FrontendConnection> connectionA;
        std::optional<frontend::FrontendConnection> connectionB;
        std::optional<frontend::FrontendConnection> rangeLegacyConnection;
        Observations observerA;
        Observations observerB;
        Observations rangeLegacyObserver;
        std::string expectedText;
        const std::string extensionAccessToken = "wire-extension-access-token-must-not-leak";
        const std::string extensionSecretAnswer = "wire-extension-secret-answer-must-not-leak";
        std::string oversizedExtensionSecret;
        frontend::SequenceNumber baselineSequence;
        std::size_t baselineMessagesA = 0;
        std::size_t baselineMessagesB = 0;
        std::size_t baselineEventsA = 0;
        std::size_t baselineEventsB = 0;
        std::size_t pendingBaselineA = 0;
        std::size_t pendingBaselineB = 0;
        std::size_t resolvedBaselineA = 0;
        std::size_t resolvedBaselineB = 0;
        frontend::SequenceNumber rangeBeforeConfig;
        frontend::SequenceNumber configSequence;
        std::size_t rangeExpandedBaseline = 0;
        std::size_t rangeLegacyBaseline = 0;
        static constexpr const char* wrapperAcquireRequestId = "wrapper-acquire";
        static constexpr const char* wrapperThreadStartRequestId = "wrapper-thread-start";
        static constexpr const char* wrapperThreadResumeRequestId = "wrapper-thread-resume";
        static constexpr const char* wrapperThreadListRequestId = "wrapper-thread-list";
        static constexpr const char* wrapperThreadReadRequestId = "wrapper-thread-read";
        static constexpr const char* wrapperTurnStartRequestId = "wrapper-turn-start";
        static constexpr const char* wrapperTurnInterruptRequestId = "wrapper-turn-interrupt";
        static constexpr const char* wrapperCursor = "wrapper-page";
        static constexpr const char* wrapperThreadId = "wrapper-thread";
        static constexpr const char* wrapperReadThreadId = "wrapper-read-thread";
        static constexpr const char* wrapperTurnId = "wrapper-turn";
        static constexpr const char* resolvedProviderRequestId = "v1-permissions-request";
        static constexpr const char* resolvedThreadId = "v1-permissions-thread";
        bool finished = false;
        std::string waitingDescription = "not started";
    };

} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    int returnCode = tests::support::cTestSkipReturnCode;

    if (tests::support::shouldSkipRootWithoutSNodeCGroup()) {
        tests::support::printRootWithoutSNodeCGroupSkipMessage("CodexFrontendServiceTest");
    } else {
        core::SNodeC::init(argc, argv);
        testCoalescingAndBounds(result);
        testAuthenticationAdmissionAndTopology(result);
        testServiceHandshakeRolesReplayAndIsolation(result);
        testCapabilityDiscoveryHandshake(result);
        testSnapshotReplayBarrier(result);
        testCapacityOnlySnapshotFeedback(result);
        bool timedOut = false;
        SparseSequenceRunner sparseRunner(result);
        FrontendBurstRunner runner(result);
        [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
            [&timedOut]() {
                timedOut = true;
                core::SNodeC::stop();
            },
            utils::Timeval({10, 0}));
        sparseRunner.start([&runner]() {
            runner.start();
        });
        const int eventLoopResult = core::SNodeC::start(utils::Timeval({12, 0}));
        result.expectTrue(!timedOut,
                          "sparse-sequence and frontend 1,000-delta scenarios complete before the watchdog (sparse stage: " +
                              sparseRunner.waitingStage() + "; burst stage: " + runner.waitingStage() + "; " + runner.terminalProgress() +
                              ")");
        result.expectTrue(sparseRunner.isFinished(), "sparse global-sequence scenario reaches a clean terminal state");
        result.expectTrue(runner.isFinished(), "frontend 1,000-delta scenario reaches a clean terminal state");
        result.expectEqual(0, eventLoopResult, "frontend service event loop exits cleanly");

        core::SNodeC::free();
        returnCode = result.processResult();
    }
    return returnCode;
}
