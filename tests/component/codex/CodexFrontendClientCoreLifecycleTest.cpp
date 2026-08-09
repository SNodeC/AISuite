/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/internal/client/ClientCore.h"
#include "support/TestResult.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace core = ai::openai::codex::frontend::internal::client;
    namespace model = ai::openai::codex::frontend::internal::model;
    namespace generated = ai::openai::codex::frontend::generated;

    core::ClientOptions clientOptions() {
        core::ClientOptions options;
        options.credentialProvider = [] {
            return core::AuthenticationContext{frontend::NoCredential{}, std::string{"lifecycle-continuity"}};
        };
        return options;
    }

    struct Harness {
        std::vector<core::OutboundMessage> outbound;
        std::vector<std::string> order;
        std::size_t closes = 0;

        core::TransportCallbacks transport() {
            return {
                [this](core::OutboundMessage message) {
                    outbound.push_back(std::move(message));
                    return core::SendResult{};
                },
                [this](std::string_view) {
                    ++closes;
                    order.emplace_back("transport-close");
                },
            };
        }
    };

    frontend::CapabilityAdvertisement capabilities() {
        std::vector<frontend::FrontendCapability> defined;
        for (const auto& metadata : generated::AllCapabilities) {
            if (metadata.defined) {
                defined.push_back(static_cast<frontend::FrontendCapability>(metadata.id));
            }
        }
        const std::vector<frontend::FrontendCapability> selected{
            frontend::FrontendCapability::CompleteBackendDomains,
            frontend::FrontendCapability::DedicatedPendingRequests,
            frontend::FrontendCapability::DedicatedNotificationEvents,
            frontend::FrontendCapability::CompleteThreadItems,
            frontend::FrontendCapability::ScopeProjectedState,
        };
        return {defined, selected, selected, frontend::Json::object()};
    }

    std::vector<frontend::FrontendMethod> methods() {
        std::vector<frontend::FrontendMethod> result;
        for (const auto& metadata : generated::AllMethods) {
            result.emplace_back(metadata.method);
        }
        return result;
    }

    frontend::Snapshot snapshot(std::uint64_t sequence) {
        model::CanonicalSnapshot canonical;
        canonical.sequence = model::FrontendSequence(sequence);
        const auto expanded = model::encodeSnapshot(canonical);
        const auto encoded = expanded ? frontend::Codec::encodeExpandedSnapshot(expanded.value())
                                      : frontend::CodecResult<frontend::Json>{frontend::CodecError{}};
        return {frontend::SequenceNumber(sequence), encoded.value().at("state")};
    }

    frontend::Welcome welcome(std::uint64_t sequence, frontend::SyncMode mode = frontend::SyncMode::Snapshot) {
        frontend::Json extensions{{"permittedScopes", frontend::Json::array({"observe", "control"})},
                                  {"projection", frontend::Json{{"profile", "test"}}}};
        return {"session-1",
                frontend::SessionRole::Observer,
                frontend::SequenceNumber(sequence),
                mode,
                std::move(extensions),
                capabilities(),
                methods(),
                methods()};
    }

    void makeReady(core::ClientCore& client, core::PhysicalGeneration generation, std::uint64_t sequence = 0) {
        (void) client.receive(generation, frontend::ServerMessage{welcome(sequence)});
        (void) client.receive(generation, frontend::ServerMessage{snapshot(sequence)});
        (void) client.receive(generation, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(sequence)}});
    }

    void testInitialStatePublicationParity(tests::support::TestResult& result) {
        Harness harness;
        std::vector<std::uint64_t> publishedRevisions;
        std::vector<core::UpdateCause> updateCauses;
        core::ClientCallbacks callbacks;
        callbacks.onStatePublished = [&publishedRevisions](const auto& state) {
            publishedRevisions.push_back(state->revision);
        };
        callbacks.onStateUpdated = [&updateCauses](const core::StateUpdate& update) {
            updateCauses.push_back(update.cause);
        };
        core::ClientCore client(clientOptions(), std::move(callbacks));
        const core::PhysicalGeneration generation = *client.attach(harness.transport());
        client.transportConnected(generation);

        const bool welcomeAccepted = client.receive(generation, frontend::ServerMessage{welcome(0)});
        const bool welcomePreservedState = welcomeAccepted && client.connectionState() == core::ConnectionState::Synchronizing &&
                                           client.state()->revision == 0 &&
                                           client.state()->freshness == core::PublishedFreshness::Stale &&
                                           !client.state()->session.has_value() && publishedRevisions.empty() && updateCauses.empty();

        const bool snapshotAccepted = client.receive(generation, frontend::ServerMessage{snapshot(0)});
        const bool snapshotPublishedOnce = snapshotAccepted && client.state()->revision == 1 &&
                                           client.state()->freshness == core::PublishedFreshness::Synchronizing &&
                                           client.state()->session.has_value() && publishedRevisions == std::vector<std::uint64_t>{1} &&
                                           updateCauses == std::vector<core::UpdateCause>{core::UpdateCause::InitialSnapshot};

        const bool completionAccepted =
            client.receive(generation, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(0)}});
        const bool completionPublishedOnce =
            completionAccepted && client.ready() && client.state()->revision == 2 &&
            client.state()->freshness == core::PublishedFreshness::Current &&
            publishedRevisions == std::vector<std::uint64_t>({1, 2}) &&
            updateCauses == std::vector<core::UpdateCause>(
                                {core::UpdateCause::InitialSnapshot, core::UpdateCause::SynchronizationCompleted});

        result.expectTrue(welcomePreservedState && snapshotPublishedOnce && completionPublishedOnce,
                          "Welcome preserves revision zero while Snapshot and SyncComplete publish exactly revisions one and two");
    }

    void testLifecycle(tests::support::TestResult& result) {
        Harness harness;
        core::ClientCallbacks callbacks;
        callbacks.onStatePublished = [&harness](const auto& state) {
            if (state->freshness == core::PublishedFreshness::Stale) {
                harness.order.emplace_back("stale");
            }
        };
        callbacks.onConnectionStateChanged = [&harness](const core::StateChange& change) {
            if (change.current == core::ConnectionState::Disconnected) {
                harness.order.emplace_back("disconnected");
            }
        };
        core::ClientCore client(clientOptions(), std::move(callbacks));
        const auto first = client.attach(harness.transport());
        result.expectTrue(first == 1 && harness.outbound.empty() && client.connectionState() == core::ConnectionState::Connecting,
                          "attachment is logical until its physical generation reports connected");

        client.transportConnected(*first);
        const auto* hello = std::get_if<frontend::Hello>(&harness.outbound.front().value);
        result.expectTrue(hello != nullptr && !hello->resumeAfter && client.connectionState() == core::ConnectionState::Authenticating,
                          "the first physical generation emits one authenticated protocol Hello without a replay cursor");

        makeReady(client, *first);
        result.expectTrue(client.ready() && client.state()->synchronizedThrough == model::FrontendSequence(0) &&
                              client.state()->projectionFingerprint.has_value(),
                          "Welcome, typed Snapshot, and SyncComplete publish a ready immutable projection with identity");
        result.expectTrue(!client.attach(harness.transport()).has_value(), "one client rejects a second simultaneous attachment");

        bool pendingFailed = false;
        const auto submitted = client.submit(generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()),
                                             [&harness, &pendingFailed](const core::OperationResult& value) {
                                                 pendingFailed = value.error.has_value();
                                                 harness.order.emplace_back("operation");
                                             });
        result.expectTrue(submitted && submitted.requestId == std::optional<std::string>{"c1-r1"},
                          "the core uses the frozen physical-generation request-ID format");
        client.transportDisconnected(*first, {"peer closed", true});
        result.expectTrue(pendingFailed && client.connectionState() == core::ConnectionState::Disconnected && harness.order.size() >= 3 &&
                              harness.order[harness.order.size() - 3] == "stale" &&
                              harness.order[harness.order.size() - 2] == "operation" && harness.order.back() == "disconnected",
                          "detach publishes stale retained state before failing pending operations and announcing Disconnected");

        const auto second = client.attach(harness.transport());
        client.transportConnected(*second);
        const auto* resumedHello = std::get_if<frontend::Hello>(&harness.outbound.back().value);
        result.expectTrue(second == 2 && resumedHello != nullptr && resumedHello->resumeAfter == frontend::SequenceNumber(0),
                          "a reusable client starts a new physical generation and resumes only its synchronized cursor");
        const std::size_t beforeStaleCallback = harness.outbound.size();
        client.transportConnected(*first);
        (void) client.receive(*first, frontend::ServerMessage{welcome(0)});
        result.expectTrue(harness.outbound.size() == beforeStaleCallback && client.activeGeneration() == second,
                          "callbacks from an old physical generation are ignored without affecting the active generation");

        makeReady(client, *second);
        const auto secondGenerationSubmission =
            client.submit(generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()));
        result.expectTrue(secondGenerationSubmission.requestId == std::optional<std::string>{"c2-r2"},
                          "request IDs retain one monotonic counter while encoding the current physical generation");

        client.close();
        client.close();
        result.expectTrue(client.connectionState() == core::ConnectionState::Closed && harness.closes == 1,
                          "terminal client close is idempotent and closes the active transport once");
    }

    void testNoContinuityDoesNotResume(tests::support::TestResult& result) {
        Harness harness;
        core::ClientOptions options = clientOptions();
        options.credentialProvider = [] {
            return core::AuthenticationContext{frontend::NoCredential{}, std::nullopt};
        };
        core::ClientCore client(std::move(options));
        const core::PhysicalGeneration first = *client.attach(harness.transport());
        client.transportConnected(first);
        makeReady(client, first);
        client.transportDisconnected(first, {"no continuity", true});
        const core::PhysicalGeneration second = *client.attach(harness.transport());
        client.transportConnected(second);
        const auto* hello = std::get_if<frontend::Hello>(&harness.outbound.back().value);
        result.expectTrue(hello != nullptr && !hello->resumeAfter,
                          "a retained cursor is not offered without an explicit matching continuity key");
    }

    void testReentrantAttachmentLifecycle(tests::support::TestResult& result) {
        Harness connectingHarness;
        core::ClientCore* connectingClient = nullptr;
        core::ClientCallbacks connectingCallbacks;
        connectingCallbacks.onConnectionStateChanged = [&connectingClient](const core::StateChange& change) {
            if (change.current == core::ConnectionState::Connecting && connectingClient != nullptr) {
                const std::optional<core::PhysicalGeneration> generation = connectingClient->activeGeneration();
                if (generation.has_value()) {
                    connectingClient->detach(*generation, "connecting callback detached");
                }
            }
        };
        core::ClientCore invalidatedAttach(clientOptions(), std::move(connectingCallbacks));
        connectingClient = &invalidatedAttach;
        const std::optional<core::PhysicalGeneration> invalidatedGeneration = invalidatedAttach.attach(connectingHarness.transport());
        result.expectTrue(!invalidatedGeneration.has_value() && !invalidatedAttach.activeGeneration().has_value() &&
                              invalidatedAttach.connectionState() == core::ConnectionState::Disconnected &&
                              connectingHarness.outbound.empty() && connectingHarness.closes == 1,
                          "a Connecting callback that detaches cannot leave attach reporting a stale physical generation");

        Harness firstHarness;
        Harness replacementHarness;
        core::ClientCore* credentialClient = nullptr;
        std::optional<core::PhysicalGeneration> firstGeneration;
        std::optional<core::PhysicalGeneration> replacementGeneration;
        core::ClientOptions options = clientOptions();
        options.credentialProvider = [&] {
            credentialClient->detach(*firstGeneration, "credential callback replaced attachment");
            replacementGeneration = credentialClient->attach(replacementHarness.transport());
            return core::AuthenticationContext{frontend::NoCredential{}, std::string{"replacement-continuity"}};
        };
        core::ClientCore replacedDuringAuthentication(std::move(options));
        credentialClient = &replacedDuringAuthentication;
        firstGeneration = replacedDuringAuthentication.attach(firstHarness.transport());
        replacedDuringAuthentication.transportConnected(*firstGeneration);
        result.expectTrue(replacementGeneration == std::optional<core::PhysicalGeneration>{2} &&
                              replacedDuringAuthentication.activeGeneration() == replacementGeneration &&
                              replacedDuringAuthentication.connectionState() == core::ConnectionState::Connecting &&
                              firstHarness.outbound.empty() && replacementHarness.outbound.empty() && firstHarness.closes == 1,
                          "credential-provider reentry cannot send the retired generation's Hello through its replacement attachment");

        Harness closingHarness;
        Harness rejectedReplacementHarness;
        core::ClientCore* closingClient = nullptr;
        std::optional<core::PhysicalGeneration> closingGeneration;
        std::optional<core::PhysicalGeneration> rejectedReplacement;
        core::ClientCallbacks closingCallbacks;
        closingCallbacks.onConnectionStateChanged = [&](const core::StateChange& change) {
            if (change.current != core::ConnectionState::Closing || closingClient == nullptr || !closingGeneration.has_value()) {
                return;
            }
            closingClient->detach(*closingGeneration, "Closing callback detach");
            closingClient->transportDisconnected(*closingGeneration, {"Closing callback disconnect", true});
            rejectedReplacement = closingClient->attach(rejectedReplacementHarness.transport());
        };
        core::ClientCore closingReentry(clientOptions(), std::move(closingCallbacks));
        closingClient = &closingReentry;
        closingGeneration = closingReentry.attach(closingHarness.transport());
        closingReentry.transportConnected(*closingGeneration);
        makeReady(closingReentry, *closingGeneration);
        closingReentry.close("terminal close");
        result.expectTrue(!rejectedReplacement.has_value() && !closingReentry.activeGeneration().has_value() &&
                              closingReentry.connectionState() == core::ConnectionState::Closed && closingHarness.closes == 1 &&
                              rejectedReplacementHarness.closes == 0 && rejectedReplacementHarness.outbound.empty(),
                          "Closing callback lifecycle reentry cannot detach, replace, or lose the terminally closing attachment");
    }

    void testPendingFailureInvalidation(tests::support::TestResult& result) {
        Harness harness;
        core::ClientCore* clientPointer = nullptr;
        core::ClientCore client(clientOptions());
        clientPointer = &client;
        const core::PhysicalGeneration generation = *client.attach(harness.transport());
        client.transportConnected(generation);
        makeReady(client, generation);

        std::size_t firstCompletions = 0;
        std::size_t staleCompletions = 0;
        const core::Submission first =
            client.submit(generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()),
                          [&](const core::OperationResult&) {
                              ++firstCompletions;
                              clientPointer->close("first failure completion closed client");
                          });
        const core::Submission second =
            client.submit(generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()),
                          [&](const core::OperationResult&) {
                              ++staleCompletions;
                          });
        client.transportDisconnected(generation, {"failure fanout", true});
        result.expectTrue(first.accepted() && second.accepted() && firstCompletions == 1 && staleCompletions == 0 &&
                              client.pendingOperationCount() == 0 && client.connectionState() == core::ConnectionState::Closed &&
                              harness.closes == 1,
                          "the first failed-operation callback may close the client and suppresses later old-generation completions");

        Harness fanoutHarness;
        core::ClientCore fanoutClient(clientOptions());
        const core::PhysicalGeneration fanoutGeneration = *fanoutClient.attach(fanoutHarness.transport());
        fanoutClient.transportConnected(fanoutGeneration);
        makeReady(fanoutClient, fanoutGeneration);
        std::size_t firstFailureCompletions = 0;
        std::size_t secondFailureCompletions = 0;
        const core::Submission fanoutFirst = fanoutClient.submit(
            generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()),
            [&](const core::OperationResult& completion) {
                firstFailureCompletions += completion.error.has_value() ? 1U : 100U;
                fanoutClient.detach(fanoutGeneration, "failure completion repeated detach");
                fanoutClient.transportDisconnected(fanoutGeneration, {"failure completion repeated disconnect", true});
            });
        const core::Submission fanoutSecond = fanoutClient.submit(
            generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()),
            [&](const core::OperationResult& completion) {
                secondFailureCompletions += completion.error.has_value() ? 1U : 100U;
            });
        const std::size_t outboundBeforeFailure = fanoutHarness.outbound.size();
        fanoutClient.transportDisconnected(fanoutGeneration, {"primary physical disconnect", true});
        result.expectTrue(fanoutFirst.accepted() && fanoutSecond.accepted() && firstFailureCompletions == 1 &&
                              secondFailureCompletions == 1 && fanoutHarness.outbound.size() == outboundBeforeFailure &&
                              fanoutHarness.closes == 0 && fanoutClient.pendingOperationCount() == 0 &&
                              !fanoutClient.activeGeneration().has_value() &&
                              fanoutClient.connectionState() == core::ConnectionState::Disconnected,
                          "detach and disconnect reentry during an existing failure fanout is idempotent, completes every already-abandoned "
                          "operation once, and never sends deferred old-generation work");
    }

    void testTerminalOrderingAndDestruction(tests::support::TestResult& result) {
        Harness harness;
        core::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&harness](const core::StateChange& change) {
            if (change.current == core::ConnectionState::Closing) {
                harness.order.emplace_back("closing");
            } else if (change.current == core::ConnectionState::Closed) {
                harness.order.emplace_back("closed");
            }
        };
        callbacks.onStateUpdated = [&harness](const core::StateUpdate& update) {
            if (update.cause == core::UpdateCause::ConnectionBecameStale) {
                harness.order.emplace_back("stale");
            }
        };
        core::ClientCore client(clientOptions(), std::move(callbacks));
        const core::PhysicalGeneration generation = *client.attach(harness.transport());
        client.transportConnected(generation);
        makeReady(client, generation);
        (void) client.submit(generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()),
                             [&harness](const core::OperationResult&) {
                                 harness.order.emplace_back("operation");
                             });
        client.close();
        result.expectTrue(harness.order.size() >= 5 && harness.order[harness.order.size() - 5] == "closing" &&
                              harness.order[harness.order.size() - 4] == "operation" &&
                              harness.order[harness.order.size() - 3] == "transport-close" &&
                              harness.order[harness.order.size() - 2] == "stale" && harness.order.back() == "closed",
                          "terminal close preserves Closing, operation, transport-close, stale-State, Closed callback ordering");

        Harness destructionHarness;
        {
            core::ClientCore destroyed(clientOptions());
            const core::PhysicalGeneration active = *destroyed.attach(destructionHarness.transport());
            destroyed.transportConnected(active);
        }
        result.expectTrue(destructionHarness.closes == 1,
                          "destroying a client with an active physical attachment performs one contained terminal transport close");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testInitialStatePublicationParity(result);
    testLifecycle(result);
    testNoContinuityDoesNotResume(result);
    testReentrantAttachmentLifecycle(result);
    testPendingFailureInvalidation(result);
    testTerminalOrderingAndDestruction(result);
    return result.processResult();
}
