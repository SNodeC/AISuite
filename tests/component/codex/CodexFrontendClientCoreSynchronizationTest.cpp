/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/internal/client/ClientCore.h"
#include "support/TestResult.h"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
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
            return core::AuthenticationContext{frontend::NoCredential{}, std::string{"synchronization-continuity"}};
        };
        return options;
    }

    struct Harness {
        std::vector<core::OutboundMessage> outbound;
        core::TransportCallbacks transport() {
            return {[this](core::OutboundMessage value) {
                        outbound.push_back(std::move(value));
                        return core::SendResult{};
                    },
                    [](std::string_view) {
                    }};
        }
    };

    std::vector<frontend::FrontendMethod> allMethods() {
        std::vector<frontend::FrontendMethod> result;
        for (const auto& method : generated::AllMethods) {
            result.emplace_back(method.method);
        }
        return result;
    }

    frontend::CapabilityAdvertisement allRepresentationCapabilities() {
        std::vector<frontend::FrontendCapability> defined;
        for (const auto& capability : generated::AllCapabilities) {
            if (capability.defined) {
                defined.push_back(static_cast<frontend::FrontendCapability>(capability.id));
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

    frontend::CapabilityAdvertisement representationCapabilities(std::vector<frontend::FrontendCapability> selected) {
        std::vector<frontend::FrontendCapability> defined;
        for (const auto& capability : generated::AllCapabilities) {
            if (capability.defined) {
                defined.push_back(static_cast<frontend::FrontendCapability>(capability.id));
            }
        }
        return {std::move(defined), selected, std::move(selected), frontend::Json::object()};
    }

    frontend::Welcome welcome(std::uint64_t sequence, frontend::SyncMode mode, std::string projection = "stable") {
        frontend::Json extensions{{"permittedScopes", frontend::Json::array({"observe", "control"})},
                                  {"projection", frontend::Json{{"identity", std::move(projection)}}}};
        return {"session-sync",
                frontend::SessionRole::Observer,
                frontend::SequenceNumber(sequence),
                mode,
                std::move(extensions),
                allRepresentationCapabilities(),
                allMethods(),
                allMethods()};
    }

    frontend::Snapshot expandedSnapshot(std::uint64_t sequence) {
        model::CanonicalSnapshot canonical;
        canonical.sequence = model::FrontendSequence(sequence);
        const auto expanded = model::encodeSnapshot(canonical);
        const auto encoded = frontend::Codec::encodeExpandedSnapshot(expanded.value());
        return {frontend::SequenceNumber(sequence), encoded.value().at("state")};
    }

    frontend::Snapshot terminalExpandedSnapshot() {
        model::CanonicalSnapshot canonical;
        canonical.sequence = model::FrontendSequence::maximum();
        canonical.backendCursor.currentSequence = canonical.sequence;
        canonical.backendCursor.frontendSequenceExhausted = true;
        const auto expanded = model::encodeSnapshot(canonical);
        const auto encoded = frontend::Codec::encodeExpandedSnapshot(expanded.value());
        return {model::FrontendSequence::maximum().protocolValue(), encoded.value().at("state")};
    }

    model::CanonicalSnapshot itemSnapshotState(std::uint64_t sequence) {
        model::CanonicalSnapshot canonical;
        canonical.sequence = model::FrontendSequence(sequence);
        canonical.provider.lifecycle = model::ProviderLifecycle::Ready;
        canonical.threads.emplace_back(model::ThreadIdentity{"partial-thread"});
        canonical.turns.emplace_back(model::TurnIdentity{"partial-turn"}, model::ThreadIdentity{"partial-thread"});
        model::ItemData item(
            model::ItemIdentity{"partial-item"}, model::ThreadIdentity{"partial-thread"}, model::TurnIdentity{"partial-turn"});
        item.status = "running";
        item.commandOutput = "initial";
        canonical.items.emplace_back(model::CommandExecutionItem{std::move(item)});
        return canonical;
    }

    frontend::Snapshot projectedSnapshot(model::CanonicalSnapshot state, const std::vector<frontend::FrontendCapability>& selected) {
        const auto projected = model::encodeProjectedSnapshot(state, selected);
        if (!projected) {
            throw std::runtime_error(projected.error().path + ": " + projected.error().message);
        }
        return projected.value();
    }

    frontend::Snapshot frozenLegacySnapshot(std::uint64_t sequence) {
        frontend::Json item{{"id", "legacy-item"},
                            {"type", "command_execution"},
                            {"status", "completed"},
                            {"commandOutput", "complete"},
                            {"data", frontend::Json{{"exitCode", 0}}},
                            {"extensions", frontend::Json::object()}};
        frontend::Json turn{{"id", "legacy-turn"},
                            {"threadId", "legacy-thread"},
                            {"status", "completed"},
                            {"active", false},
                            {"terminal", true},
                            {"items", frontend::Json::array({std::move(item)})},
                            {"extensions", frontend::Json::object()}};
        frontend::Json thread{{"id", "legacy-thread"},
                              {"fullyLoaded", true},
                              {"turns", frontend::Json::array({std::move(turn)})},
                              {"extensions", frontend::Json::object()}};
        frontend::Json state{{"backendRevision", std::uint64_t{7}},
                             {"lifecycle", "ready"},
                             {"diagnostics", {{"received", std::uint64_t{0}}, {"recent", frontend::Json::array()}}},
                             {"controllerSessionId", "1"},
                             {"sessions", frontend::Json::array()},
                             {"threadList", {{"hasLoadedPage", true}, {"complete", true}, {"pagesLoaded", std::uint64_t{1}}}},
                             {"threads", frontend::Json::array({std::move(thread)})},
                             {"pendingRequests", frontend::Json::array()},
                             {"codexExtensions", frontend::Json::array()},
                             {"omittedCodexExtensions", std::uint64_t{0}},
                             {"journal", {{"oldestReplayableAfter", std::uint64_t{0}}, {"currentSequence", std::uint64_t{7}}}},
                             {"sequenceExhausted", false}};
        return {frontend::SequenceNumber(sequence), std::move(state)};
    }

    frontend::FrontendEvent providerEvent(std::uint64_t sequence) {
        model::ProviderState provider;
        provider.lifecycle = model::ProviderLifecycle::Ready;
        model::OccurrenceIdentity identity{
            model::FrontendSequence(sequence), model::OccurrenceGroupIdentity{"sync-group"}, 0, 1, model::SourceStamp{"sync-source"}};
        auto occurrence = model::makeOccurrence(std::move(identity), model::ProviderUpdatedOccurrence{std::move(provider)});
        if (!occurrence) {
            throw std::runtime_error(occurrence.error().path + ": " + occurrence.error().message);
        }
        auto expanded = model::encodeExpandedOccurrence(occurrence.value());
        if (!expanded) {
            throw std::runtime_error(expanded.error().path + ": " + expanded.error().message);
        }
        const frontend::ExpandedFrontendEvent& event = expanded.value().front();
        return {event.sequence, std::string(frontend::toString(event.type)), event.data, event.extensions};
    }

    frontend::FrontendEvent occurrenceEvent(std::uint64_t sequence, model::OccurrencePayload payload, bool expanded) {
        model::OccurrenceIdentity identity{model::FrontendSequence(sequence),
                                           model::OccurrenceGroupIdentity{"representation-group-" + std::to_string(sequence)},
                                           0,
                                           1,
                                           model::SourceStamp{"representation-source-" + std::to_string(sequence)}};
        auto occurrence = model::makeOccurrence(std::move(identity), std::move(payload));
        if (!occurrence) {
            throw std::runtime_error(occurrence.error().path + ": " + occurrence.error().message);
        }
        if (!expanded) {
            const auto encoded = model::encodeLegacyOccurrence(occurrence.value());
            if (!encoded) {
                throw std::runtime_error(encoded.error().path + ": " + encoded.error().message);
            }
            return encoded.value();
        }
        const auto encoded = model::encodeExpandedOccurrence(occurrence.value());
        if (!encoded) {
            throw std::runtime_error(encoded.error().path + ": " + encoded.error().message);
        }
        const frontend::ExpandedFrontendEvent& event = encoded.value().front();
        return {event.sequence, std::string(frontend::toString(event.type)), event.data, event.extensions};
    }

    frontend::FrontendEvent diagnosticEvent(std::uint64_t sequence, bool expanded) {
        model::DiagnosticRecord diagnostic;
        diagnostic.received = sequence;
        diagnostic.detailsOmitted = true;
        diagnostic.message = expanded ? "expanded diagnostic" : "legacy diagnostic";
        return occurrenceEvent(sequence, model::DiagnosticsUpdatedOccurrence{std::move(diagnostic)}, expanded);
    }

    frontend::FrontendEvent itemContentEvent(std::uint64_t sequence, bool expanded) {
        model::ItemContentUpdatedOccurrence content(model::ItemIdentity{"partial-item"});
        content.threadId = model::ThreadIdentity{"partial-thread"};
        content.turnId = model::TurnIdentity{"partial-turn"};
        content.channel = "commandOutput";
        content.content = expanded ? "expanded content" : "legacy content";
        return occurrenceEvent(sequence, std::move(content), expanded);
    }

    frontend::FrontendEvent itemContentAppendEvent(std::uint64_t sequence,
                                                   std::uint64_t baseContentBytes,
                                                   std::string delta) {
        return {frontend::SequenceNumber{sequence},
                "item.content.updated",
                {{"threadId", "partial-thread"},
                 {"turnId", "partial-turn"},
                 {"itemId", "partial-item"},
                 {"channel", "commandOutput"},
                 {"content", ""},
                 {"contentDelta", std::move(delta)},
                 {"baseContentBytes", baseContentBytes},
                 {"contentTruncated", false},
                 {"droppedContentBytes", std::uint64_t{0}}}};
    }

    core::PhysicalGeneration
    readyWithCapabilities(core::ClientCore& client,
                          Harness& harness,
                          const std::vector<frontend::FrontendCapability>& selected,
                          bool selectItemContentAppend = false) {
        const core::PhysicalGeneration generation = *client.attach(harness.transport());
        client.transportConnected(generation);
        frontend::Json extensions{{"permittedScopes", frontend::Json::array({"observe"})}};
        if (selectItemContentAppend) {
            extensions["projection"] = {{"itemContentUpdateMode", "append-v1"}};
        }
        frontend::Welcome selectedWelcome{"partial-session",
                                          frontend::SessionRole::Observer,
                                          frontend::SequenceNumber(0),
                                          frontend::SyncMode::Snapshot,
                                          std::move(extensions),
                                          representationCapabilities(selected),
                                          allMethods(),
                                          allMethods()};
        (void) client.receive(generation, frontend::ServerMessage{selectedWelcome});
        (void) client.receive(generation, frontend::ServerMessage{projectedSnapshot(itemSnapshotState(0), selected)});
        (void) client.receive(generation, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(0)}});
        return generation;
    }

    void initialReady(core::ClientCore& client, core::PhysicalGeneration generation, std::uint64_t sequence = 0) {
        (void) client.receive(generation, frontend::ServerMessage{welcome(sequence, frontend::SyncMode::Snapshot)});
        (void) client.receive(generation, frontend::ServerMessage{expandedSnapshot(sequence)});
        (void) client.receive(generation, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(sequence)}});
    }

    void testSnapshotReplayAndProjectionRefresh(tests::support::TestResult& result) {
        Harness harness;
        std::size_t stateUpdates = 0;
        core::ClientCallbacks callbacks;
        callbacks.onStateUpdated = [&stateUpdates](const core::StateUpdate&) {
            ++stateUpdates;
        };
        core::ClientCore client(clientOptions(), std::move(callbacks));
        const auto first = client.attach(harness.transport());
        client.transportConnected(*first);
        initialReady(client, *first);

        client.transportDisconnected(*first, {"reconnect", true});
        const auto second = client.attach(harness.transport());
        client.transportConnected(*second);
        (void) client.receive(*second, frontend::ServerMessage{welcome(1, frontend::SyncMode::Replay)});
        frontend::FrontendEvent event = providerEvent(1);
        (void) client.receive(*second, frontend::ServerMessage{frontend::EventBatch{event.sequence, event.sequence, {std::move(event)}}});
        (void) client.receive(*second, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(1)}});
        result.expectTrue(client.ready() && client.state()->visibleSequence == model::FrontendSequence(1) &&
                              client.state()->snapshot->provider.ready(),
                          "reconnect replay reduces typed occurrences over retained stale state before publishing Ready");

        const std::string oldFingerprint = *client.state()->projectionFingerprint;
        client.transportDisconnected(*second, {"projection changed", true});
        const auto third = client.attach(harness.transport());
        client.transportConnected(*third);
        (void) client.receive(*third, frontend::ServerMessage{welcome(1, frontend::SyncMode::Replay, "changed")});
        const std::size_t beforeRefresh = harness.outbound.size();
        const std::size_t beforeValidationUpdates = stateUpdates;
        (void) client.receive(*third, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(1)}});
        const bool snapshotRequested =
            harness.outbound.size() == beforeRefresh + 1 && harness.outbound.back().isCommand() &&
            generated::commandMethod(std::get<generated::DefinedCommand>(harness.outbound.back().value).parameters) ==
                generated::MethodId::SnapshotGet;
        result.expectTrue(
            snapshotRequested && client.connectionState() == core::ConnectionState::Synchronizing &&
                stateUpdates == beforeValidationUpdates,
            "changed replay projection identity remains validation-only and triggers a bounded snapshot refresh before Ready");

        const std::string refreshId = std::get<generated::DefinedCommand>(harness.outbound.back().value).requestId;
        (void) client.receive(*third, frontend::ServerMessage{frontend::Response::success(refreshId, frontend::Json{{"sequence", 1}})});
        (void) client.receive(*third, frontend::ServerMessage{expandedSnapshot(1)});
        (void) client.receive(*third, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(1)}});
        result.expectTrue(client.ready() && client.state()->projectionFingerprint != std::optional<std::string>{oldFingerprint},
                          "projection refresh publishes only the new canonical fingerprint and refreshed Snapshot");

        Harness restartedHarness;
        std::size_t restartedSnapshotFallbacks = 0;
        core::ClientCallbacks restartedCallbacks;
        restartedCallbacks.onStateUpdated = [&restartedSnapshotFallbacks](const core::StateUpdate& update) {
            restartedSnapshotFallbacks += update.cause == core::UpdateCause::SnapshotFallback ? 1U : 0U;
        };
        core::ClientCore restarted(clientOptions(), std::move(restartedCallbacks));
        const auto restartedFirst = restarted.attach(restartedHarness.transport());
        restarted.transportConnected(*restartedFirst);
        initialReady(restarted, *restartedFirst, 7);
        restarted.transportDisconnected(*restartedFirst, {"server restarted", true});
        const auto restartedSecond = restarted.attach(restartedHarness.transport());
        restarted.transportConnected(*restartedSecond);
        const auto* futureHello = std::get_if<frontend::Hello>(&restartedHarness.outbound.back().value);
        const bool lowerWelcomeAccepted =
            restarted.receive(*restartedSecond, frontend::ServerMessage{welcome(1, frontend::SyncMode::Snapshot)});
        const bool lowerSnapshotAccepted =
            restarted.receive(*restartedSecond, frontend::ServerMessage{expandedSnapshot(1)});
        const bool lowerCompleteAccepted = restarted.receive(
            *restartedSecond, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(1)}});
        result.expectTrue(futureHello != nullptr && futureHello->resumeAfter == frontend::SequenceNumber(7) &&
                              lowerWelcomeAccepted && lowerSnapshotAccepted && lowerCompleteAccepted && restarted.ready() &&
                              restarted.state()->snapshot && restarted.state()->visibleSequence == model::FrontendSequence(1) &&
                              restarted.state()->synchronizedThrough == model::FrontendSequence(1) &&
                              restartedSnapshotFallbacks == 1,
                          "a restarted server replaces a future retained cursor with its authoritative lower Snapshot fallback");
        restarted.transportDisconnected(*restartedSecond, {"verify replacement cursor", true});
        const auto restartedThird = restarted.attach(restartedHarness.transport());
        restarted.transportConnected(*restartedThird);
        const auto* replacementHello = std::get_if<frontend::Hello>(&restartedHarness.outbound.back().value);
        result.expectTrue(replacementHello != nullptr && replacementHello->resumeAfter == frontend::SequenceNumber(1),
                          "the next physical generation resumes from the restarted server's replacement Snapshot boundary");

        Harness regressedReplayHarness;
        core::ClientCore regressedReplay(clientOptions());
        const auto replayFirst = regressedReplay.attach(regressedReplayHarness.transport());
        regressedReplay.transportConnected(*replayFirst);
        initialReady(regressedReplay, *replayFirst, 7);
        regressedReplay.transportDisconnected(*replayFirst, {"replay regression", true});
        const auto replaySecond = regressedReplay.attach(regressedReplayHarness.transport());
        regressedReplay.transportConnected(*replaySecond);
        const bool regressedReplayAccepted =
            regressedReplay.receive(*replaySecond, frontend::ServerMessage{welcome(1, frontend::SyncMode::Replay)});
        result.expectTrue(!regressedReplayAccepted && regressedReplay.connectionState() == core::ConnectionState::Disconnected,
                          "a Replay Welcome still cannot regress behind the requested retained cursor");
    }

    void testLegacyAndExplicitOrdering(tests::support::TestResult& result) {
        Harness legacyHarness;
        core::ClientOptions legacyOptions = clientOptions();
        legacyOptions.requestedCapabilities.clear();
        core::ClientCore legacy(std::move(legacyOptions));
        const auto generation = legacy.attach(legacyHarness.transport());
        legacy.transportConnected(*generation);
        frontend::Welcome legacyWelcome{
            "legacy-session", frontend::SessionRole::Observer, frontend::SequenceNumber(0), frontend::SyncMode::Snapshot};
        const frontend::Snapshot legacyFixture = frozenLegacySnapshot(0);
        const auto directlyDecoded = model::decodeProjectedSnapshot(legacyFixture, std::span<const frontend::FrontendCapability>{});
        result.expectTrue(directlyDecoded.hasValue(),
                          directlyDecoded.hasValue() ? "the frozen legacy fixture has one typed canonical decoding"
                                                     : "the frozen legacy fixture failed at " + directlyDecoded.error().path + ": " +
                                                           directlyDecoded.error().message);
        (void) legacy.receive(*generation, frontend::ServerMessage{legacyWelcome});
        (void) legacy.receive(*generation, frontend::ServerMessage{legacyFixture});
        (void) legacy.receive(*generation, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(0)}});
        result.expectTrue(legacy.ready() && legacy.state()->representation == core::RepresentationMode::LegacyV1 &&
                              legacy.state()->snapshot->provider.ready() &&
                              legacy.state()->snapshot->backendCursor.backendRevision == std::uint64_t{7} &&
                              legacy.state()->snapshot->backendCursor.currentSequence == model::FrontendSequence(7) &&
                              legacy.state()->item("legacy-item") != nullptr &&
                              model::threadItemKind(*legacy.state()->item("legacy-item")) == frontend::ThreadItemKind::CommandExecution,
                          "the frozen nested legacy v1 state shape reduces without aliasing expanded fields");

        Harness explicitHarness;
        core::ClientCore explicitClient(clientOptions());
        const auto explicitGeneration = explicitClient.attach(explicitHarness.transport());
        explicitClient.transportConnected(*explicitGeneration);
        initialReady(explicitClient, *explicitGeneration);
        const core::Submission submission = explicitClient.requestSnapshot();
        (void) explicitClient.receive(
            *explicitGeneration,
            frontend::ServerMessage{frontend::Response::success(*submission.requestId, frontend::Json{{"sequence", 0}})});
        (void) explicitClient.receive(*explicitGeneration, frontend::ServerMessage{expandedSnapshot(0)});
        (void) explicitClient.receive(*explicitGeneration, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(0)}});
        result.expectTrue(submission && explicitClient.ready(),
                          "explicit snapshot synchronization requires its correlated command response before stream completion");
    }

    void testSynchronizationPublicationsAndGuards(tests::support::TestResult& result) {
        Harness publicationHarness;
        std::vector<std::pair<core::UpdateCause, core::PublishedFreshness>> updates;
        core::ClientCallbacks callbacks;
        callbacks.onStateUpdated = [&updates](const core::StateUpdate& update) {
            updates.emplace_back(update.cause, update.state->freshness);
        };
        core::ClientCore publicationClient(clientOptions(), std::move(callbacks));
        const auto publicationGeneration = publicationClient.attach(publicationHarness.transport());
        publicationClient.transportConnected(*publicationGeneration);
        (void) publicationClient.receive(*publicationGeneration, frontend::ServerMessage{welcome(0, frontend::SyncMode::Snapshot)});
        (void) publicationClient.receive(*publicationGeneration, frontend::ServerMessage{expandedSnapshot(0)});
        const bool stagedPublished = updates.size() == 1 && updates.front().first == core::UpdateCause::InitialSnapshot &&
                                     updates.front().second == core::PublishedFreshness::Synchronizing;
        (void) publicationClient.receive(*publicationGeneration,
                                         frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(0)}});
        result.expectTrue(stagedPublished && updates.size() == 2 && updates.back().first == core::UpdateCause::SynchronizationCompleted &&
                              updates.back().second == core::PublishedFreshness::Current,
                          "accepted synchronization data publishes an immutable Synchronizing revision before the Ready boundary");

        Harness missingHarness;
        core::ClientCore missing(clientOptions());
        const auto missingGeneration = missing.attach(missingHarness.transport());
        missing.transportConnected(*missingGeneration);
        (void) missing.receive(*missingGeneration, frontend::ServerMessage{welcome(0, frontend::SyncMode::Snapshot)});
        (void) missing.receive(*missingGeneration, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(0)}});

        Harness duplicateHarness;
        core::ClientCore duplicate(clientOptions());
        const auto duplicateGeneration = duplicate.attach(duplicateHarness.transport());
        duplicate.transportConnected(*duplicateGeneration);
        (void) duplicate.receive(*duplicateGeneration, frontend::ServerMessage{welcome(0, frontend::SyncMode::Snapshot)});
        (void) duplicate.receive(*duplicateGeneration, frontend::ServerMessage{expandedSnapshot(0)});
        (void) duplicate.receive(*duplicateGeneration, frontend::ServerMessage{expandedSnapshot(0)});

        Harness mixedHarness;
        core::ClientCore mixed(clientOptions());
        const auto mixedGeneration = mixed.attach(mixedHarness.transport());
        mixed.transportConnected(*mixedGeneration);
        (void) mixed.receive(*mixedGeneration, frontend::ServerMessage{welcome(1, frontend::SyncMode::Replay)});
        (void) mixed.receive(*mixedGeneration, frontend::ServerMessage{expandedSnapshot(1)});
        result.expectTrue(missing.connectionState() == core::ConnectionState::Disconnected &&
                              duplicate.connectionState() == core::ConnectionState::Disconnected &&
                              mixed.connectionState() == core::ConnectionState::Disconnected,
                          "missing, duplicate, and mixed synchronization streams are deterministic protocol failures");
    }

    void testProjectedSequenceRules(tests::support::TestResult& result) {
        Harness gapHarness;
        core::ClientCore gapClient(clientOptions());
        const auto gapGeneration = gapClient.attach(gapHarness.transport());
        gapClient.transportConnected(*gapGeneration);
        initialReady(gapClient, *gapGeneration);
        frontend::FrontendEvent gap = providerEvent(3);
        const bool gapAccepted =
            gapClient.receive(*gapGeneration, frontend::ServerMessage{frontend::EventBatch{gap.sequence, gap.sequence, {std::move(gap)}}});
        result.expectTrue(gapAccepted && gapClient.state()->visibleSequence == model::FrontendSequence(3),
                          "projected live occurrences may advance across omitted global sequence values");

        Harness replayHarness;
        std::optional<core::SynchronizationInfo> replayInfo;
        core::ClientCallbacks replayCallbacks;
        replayCallbacks.onSynchronized = [&replayInfo](const core::SynchronizationInfo& info) {
            replayInfo = info;
        };
        core::ClientCore replayClient(clientOptions(), std::move(replayCallbacks));
        const auto first = replayClient.attach(replayHarness.transport());
        replayClient.transportConnected(*first);
        initialReady(replayClient, *first, 1);
        replayClient.transportDisconnected(*first, {"replay", true});
        const auto second = replayClient.attach(replayHarness.transport());
        replayClient.transportConnected(*second);
        (void) replayClient.receive(*second, frontend::ServerMessage{welcome(5, frontend::SyncMode::Replay)});
        frontend::FrontendEvent ignored = providerEvent(1);
        frontend::FrontendEvent applied = providerEvent(4);
        (void) replayClient.receive(
            *second,
            frontend::ServerMessage{frontend::EventBatch{ignored.sequence, applied.sequence, {std::move(ignored), std::move(applied)}}});
        (void) replayClient.receive(*second, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(5)}});
        result.expectTrue(
            replayClient.ready() && replayClient.state()->visibleSequence == model::FrontendSequence(4) &&
                replayClient.state()->synchronizedThrough == model::FrontendSequence(5) && replayInfo.has_value() &&
                replayInfo->appliedOccurrences == 1 && replayInfo->ignoredOccurrences == 1,
            "replay ignores only the fixed represented prefix and preserves visible below a projected synchronization cursor");

        Harness splitHarness;
        core::ClientCore splitClient(clientOptions());
        const auto splitFirst = splitClient.attach(splitHarness.transport());
        splitClient.transportConnected(*splitFirst);
        initialReady(splitClient, *splitFirst);
        splitClient.transportDisconnected(*splitFirst, {"split", true});
        const auto splitSecond = splitClient.attach(splitHarness.transport());
        splitClient.transportConnected(*splitSecond);
        (void) splitClient.receive(*splitSecond, frontend::ServerMessage{welcome(3, frontend::SyncMode::Replay)});
        frontend::FrontendEvent firstMember = providerEvent(2);
        (void) splitClient.receive(
            *splitSecond,
            frontend::ServerMessage{frontend::EventBatch{firstMember.sequence, firstMember.sequence, {std::move(firstMember)}}});
        frontend::FrontendEvent splitMember = providerEvent(2);
        (void) splitClient.receive(
            *splitSecond,
            frontend::ServerMessage{frontend::EventBatch{splitMember.sequence, splitMember.sequence, {std::move(splitMember)}}});
        result.expectTrue(splitClient.connectionState() == core::ConnectionState::Disconnected,
                          "replay batch overlap cannot split an equal-sequence occurrence group across batches");
    }

    void testLiveSnapshotCursorRules(tests::support::TestResult& result) {
        Harness harness;
        std::size_t cursors = 0;
        core::ClientCallbacks callbacks;
        callbacks.onCursorAdvanced = [&cursors](model::FrontendSequence) {
            ++cursors;
        };
        core::ClientCore client(clientOptions(), std::move(callbacks));
        const auto generation = client.attach(harness.transport());
        client.transportConnected(*generation);
        (void) client.receive(*generation, frontend::ServerMessage{welcome(2, frontend::SyncMode::Snapshot)});
        (void) client.receive(*generation, frontend::ServerMessage{expandedSnapshot(2)});
        (void) client.receive(*generation, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(2)}});
        const std::uint64_t priorRevision = client.state()->revision;
        const std::size_t priorCursors = cursors;
        const bool equalAccepted = client.receive(*generation, frontend::ServerMessage{expandedSnapshot(2)});
        const bool equalReplaced = equalAccepted && client.state()->revision > priorRevision && cursors == priorCursors;
        (void) client.receive(*generation, frontend::ServerMessage{expandedSnapshot(1)});
        result.expectTrue(equalReplaced && client.connectionState() == core::ConnectionState::Disconnected,
                          "equal live Snapshot replaces State without cursor notification while a lower Snapshot closes for divergence");

        Harness terminalHarness;
        core::ClientCore terminalClient(clientOptions());
        const auto terminalGeneration = terminalClient.attach(terminalHarness.transport());
        terminalClient.transportConnected(*terminalGeneration);
        (void) terminalClient.receive(
            *terminalGeneration,
            frontend::ServerMessage{welcome(model::FrontendSequence::maximum().value(), frontend::SyncMode::Snapshot)});
        (void) terminalClient.receive(*terminalGeneration, frontend::ServerMessage{terminalExpandedSnapshot()});
        (void) terminalClient.receive(
            *terminalGeneration,
            frontend::ServerMessage{frontend::SyncComplete{model::FrontendSequence::maximum().protocolValue()}});
        const std::uint64_t terminalRevision = terminalClient.state()->revision;
        const bool terminalAccepted =
            terminalClient.receive(*terminalGeneration, frontend::ServerMessage{terminalExpandedSnapshot()});
        result.expectTrue(terminalAccepted && terminalClient.ready() && terminalClient.state()->revision == terminalRevision + 1 &&
                              terminalClient.state()->visibleSequence == model::FrontendSequence::maximum() &&
                              terminalClient.state()->snapshot->backendCursor.frontendSequenceExhausted,
                          "the frozen same-sequence terminal live Snapshot is accepted with explicit exhaustion metadata");
    }

    void testLiveCallbackLifecycleInvalidation(tests::support::TestResult& result) {
        {
            Harness harness;
            core::ClientCore* client = nullptr;
            bool closeOnPublication = false;
            std::size_t snapshotUpdates = 0;
            std::size_t cursors = 0;
            std::size_t protocolMessages = 0;
            core::ClientCallbacks callbacks;
            callbacks.onStatePublished = [&](const std::shared_ptr<const core::PublishedState>& state) {
                if (closeOnPublication && state->freshness == core::PublishedFreshness::Current &&
                    state->visibleSequence == model::FrontendSequence{1}) {
                    closeOnPublication = false;
                    client->close("live Snapshot publication callback closed");
                }
            };
            callbacks.onStateUpdated = [&snapshotUpdates](const core::StateUpdate& update) {
                snapshotUpdates += update.cause == core::UpdateCause::SnapshotFallback ? 1U : 0U;
            };
            callbacks.onCursorAdvanced = [&cursors](model::FrontendSequence) {
                ++cursors;
            };
            callbacks.onProtocolMessage = [&protocolMessages](const frontend::ServerMessage&) {
                ++protocolMessages;
            };
            core::ClientCore snapshotClient(clientOptions(), std::move(callbacks));
            client = &snapshotClient;
            const core::PhysicalGeneration generation = *snapshotClient.attach(harness.transport());
            snapshotClient.transportConnected(generation);
            initialReady(snapshotClient, generation);
            snapshotUpdates = 0;
            cursors = 0;
            protocolMessages = 0;
            closeOnPublication = true;
            (void) snapshotClient.receive(generation, frontend::ServerMessage{expandedSnapshot(1)});
            result.expectTrue(snapshotClient.connectionState() == core::ConnectionState::Closed && snapshotUpdates == 0 && cursors == 0 &&
                                  protocolMessages == 0,
                              "closing in live Snapshot publication stops its state-update, cursor, and ordinary protocol callbacks");
        }

        {
            Harness firstHarness;
            Harness replacementHarness;
            core::ClientCore* client = nullptr;
            core::PhysicalGeneration firstGeneration = 0;
            std::optional<core::PhysicalGeneration> replacementGeneration;
            bool disconnectOnPublication = false;
            std::size_t liveUpdates = 0;
            std::size_t cursors = 0;
            std::size_t protocolMessages = 0;
            core::ClientCallbacks callbacks;
            callbacks.onStatePublished = [&](const std::shared_ptr<const core::PublishedState>& state) {
                if (disconnectOnPublication && state->freshness == core::PublishedFreshness::Current &&
                    state->visibleSequence == model::FrontendSequence{1}) {
                    disconnectOnPublication = false;
                    client->transportDisconnected(firstGeneration, {"live event publication callback disconnected", true});
                    replacementGeneration = client->attach(replacementHarness.transport());
                }
            };
            callbacks.onStateUpdated = [&liveUpdates](const core::StateUpdate& update) {
                liveUpdates += update.cause == core::UpdateCause::Live ? 1U : 0U;
            };
            callbacks.onCursorAdvanced = [&cursors](model::FrontendSequence) {
                ++cursors;
            };
            callbacks.onProtocolMessage = [&protocolMessages](const frontend::ServerMessage&) {
                ++protocolMessages;
            };
            core::ClientCore eventClient(clientOptions(), std::move(callbacks));
            client = &eventClient;
            firstGeneration = *eventClient.attach(firstHarness.transport());
            eventClient.transportConnected(firstGeneration);
            initialReady(eventClient, firstGeneration);
            liveUpdates = 0;
            cursors = 0;
            protocolMessages = 0;
            disconnectOnPublication = true;
            frontend::FrontendEvent event = providerEvent(1);
            (void) eventClient.receive(firstGeneration,
                                       frontend::ServerMessage{frontend::EventBatch{event.sequence, event.sequence, {std::move(event)}}});
            frontend::ProtocolErrorMessage staleMessage;
            staleMessage.code = frontend::ErrorCode::RateLimited;
            staleMessage.message = "retired generation";
            staleMessage.closeConnection = false;
            const bool staleRejected = !eventClient.receive(firstGeneration, frontend::ServerMessage{std::move(staleMessage)});
            result.expectTrue(replacementGeneration == std::optional<core::PhysicalGeneration>{2} &&
                                  eventClient.activeGeneration() == replacementGeneration &&
                                  eventClient.connectionState() == core::ConnectionState::Connecting && liveUpdates == 0 && cursors == 0 &&
                                  protocolMessages == 0 && staleRejected,
                              "disconnecting and reattaching in live event publication stops old-generation continuation");
        }

        {
            Harness harness;
            core::ClientCore* client = nullptr;
            bool closeOnUpdate = false;
            std::size_t cursors = 0;
            std::size_t protocolMessages = 0;
            core::ClientCallbacks callbacks;
            callbacks.onStateUpdated = [&](const core::StateUpdate& update) {
                if (closeOnUpdate && update.cause == core::UpdateCause::Live) {
                    closeOnUpdate = false;
                    client->close("live StateUpdate callback closed");
                }
            };
            callbacks.onCursorAdvanced = [&cursors](model::FrontendSequence) {
                ++cursors;
            };
            callbacks.onProtocolMessage = [&protocolMessages](const frontend::ServerMessage&) {
                ++protocolMessages;
            };
            core::ClientCore updateClient(clientOptions(), std::move(callbacks));
            client = &updateClient;
            const core::PhysicalGeneration generation = *updateClient.attach(harness.transport());
            updateClient.transportConnected(generation);
            initialReady(updateClient, generation);
            cursors = 0;
            protocolMessages = 0;
            closeOnUpdate = true;
            frontend::FrontendEvent event = providerEvent(1);
            (void) updateClient.receive(generation,
                                        frontend::ServerMessage{frontend::EventBatch{event.sequence, event.sequence, {std::move(event)}}});
            result.expectTrue(updateClient.connectionState() == core::ConnectionState::Closed && cursors == 0 && protocolMessages == 0,
                              "closing in a live StateUpdate callback prevents a later cursor or ordinary protocol callback");
        }

        {
            Harness harness;
            core::ClientCore* client = nullptr;
            bool closeOnCursor = false;
            std::size_t cursors = 0;
            std::size_t protocolMessages = 0;
            core::ClientCallbacks callbacks;
            callbacks.onCursorAdvanced = [&](model::FrontendSequence sequence) {
                if (closeOnCursor && sequence == model::FrontendSequence{1}) {
                    closeOnCursor = false;
                    ++cursors;
                    client->close("live cursor callback closed");
                }
            };
            callbacks.onProtocolMessage = [&protocolMessages](const frontend::ServerMessage&) {
                ++protocolMessages;
            };
            core::ClientCore cursorClient(clientOptions(), std::move(callbacks));
            client = &cursorClient;
            const core::PhysicalGeneration generation = *cursorClient.attach(harness.transport());
            cursorClient.transportConnected(generation);
            initialReady(cursorClient, generation);
            protocolMessages = 0;
            closeOnCursor = true;
            frontend::FrontendEvent event = providerEvent(1);
            const bool accepted = cursorClient.receive(
                generation, frontend::ServerMessage{frontend::EventBatch{event.sequence, event.sequence, {std::move(event)}}});
            result.expectTrue(!accepted && cursorClient.connectionState() == core::ConnectionState::Closed && cursors == 1 &&
                                  protocolMessages == 0,
                              "closing in a cursor callback prevents the old generation's ordinary protocol callback");
        }

        {
            Harness harness;
            core::ClientCore* client = nullptr;
            std::size_t synchronized = 0;
            std::size_t protocolMessages = 0;
            core::ClientCallbacks callbacks;
            callbacks.onSynchronized = [&](const core::SynchronizationInfo&) {
                ++synchronized;
                client->close("synchronization callback closed");
            };
            callbacks.onProtocolMessage = [&protocolMessages](const frontend::ServerMessage&) {
                ++protocolMessages;
            };
            core::ClientCore synchronizationClient(clientOptions(), std::move(callbacks));
            client = &synchronizationClient;
            const core::PhysicalGeneration generation = *synchronizationClient.attach(harness.transport());
            synchronizationClient.transportConnected(generation);
            (void) synchronizationClient.receive(generation,
                                                 frontend::ServerMessage{welcome(0, frontend::SyncMode::Snapshot)});
            (void) synchronizationClient.receive(generation, frontend::ServerMessage{expandedSnapshot(0)});
            protocolMessages = 0;
            const bool accepted = synchronizationClient.receive(
                generation, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(0)}});
            result.expectTrue(!accepted && synchronizationClient.connectionState() == core::ConnectionState::Closed && synchronized == 1 &&
                                  protocolMessages == 0,
                              "closing in a synchronization callback prevents later old-generation continuation");
        }

        {
            Harness harness;
            core::ClientCore* client = nullptr;
            std::size_t errors = 0;
            std::size_t diagnostics = 0;
            std::size_t protocolMessages = 0;
            core::ClientCallbacks callbacks;
            callbacks.onError = [&](const core::ClientError&) {
                ++errors;
                client->close("non-closing error callback closed");
            };
            callbacks.onDiagnostic = [&diagnostics](const core::Diagnostic&) {
                ++diagnostics;
            };
            callbacks.onProtocolMessage = [&protocolMessages](const frontend::ServerMessage&) {
                ++protocolMessages;
            };
            core::ClientCore errorClient(clientOptions(), std::move(callbacks));
            client = &errorClient;
            const core::PhysicalGeneration generation = *errorClient.attach(harness.transport());
            errorClient.transportConnected(generation);
            initialReady(errorClient, generation);
            protocolMessages = 0;
            frontend::ProtocolErrorMessage warning;
            warning.code = frontend::ErrorCode::RateLimited;
            warning.message = "non-closing error callback invalidation";
            warning.closeConnection = false;
            const bool accepted = errorClient.receive(generation, frontend::ServerMessage{std::move(warning)});
            result.expectTrue(!accepted && errorClient.connectionState() == core::ConnectionState::Closed && errors == 1 && diagnostics == 0 &&
                                  protocolMessages == 0,
                              "closing in an error callback suppresses its diagnostic and ordinary protocol continuations");
        }

        {
            Harness harness;
            std::size_t fatalProtocolObservations = 0;
            core::ClientCallbacks callbacks;
            callbacks.onProtocolMessage = [&fatalProtocolObservations](const frontend::ServerMessage& message) {
                const auto* error = std::get_if<frontend::ProtocolErrorMessage>(&message);
                fatalProtocolObservations += error != nullptr && error->closeConnection ? 1U : 0U;
            };
            core::ClientCore protocolClient(clientOptions(), std::move(callbacks));
            const core::PhysicalGeneration generation = *protocolClient.attach(harness.transport());
            protocolClient.transportConnected(generation);
            initialReady(protocolClient, generation);
            frontend::ProtocolErrorMessage fatal;
            fatal.code = frontend::ErrorCode::InvalidCommand;
            fatal.message = "terminal protocol observation";
            fatal.closeConnection = true;
            (void) protocolClient.receive(generation, frontend::ServerMessage{std::move(fatal)});
            result.expectTrue(protocolClient.connectionState() == core::ConnectionState::Disconnected &&
                                  fatalProtocolObservations == 1,
                              "the intentional terminal ProtocolError observation remains visible exactly once after closure");
        }
    }

    void testClosingProtocolErrorAcceptance(tests::support::TestResult& result) {
        struct Case {
            frontend::ErrorCode code;
            bool retryable;
        };
        constexpr std::array cases{
            Case{frontend::ErrorCode::InternalError, true},
            Case{frontend::ErrorCode::InvalidCommand, false},
        };

        for (const Case& testCase : cases) {
            Harness harness;
            std::vector<core::StateChange> stateChanges;
            std::size_t protocolMessages = 0;
            core::ClientCallbacks callbacks;
            callbacks.onConnectionStateChanged = [&stateChanges](const core::StateChange& change) {
                stateChanges.push_back(change);
            };
            callbacks.onProtocolMessage = [&protocolMessages](const frontend::ServerMessage&) {
                ++protocolMessages;
            };
            core::ClientCore client(clientOptions(), std::move(callbacks));
            const core::PhysicalGeneration generation = *client.attach(harness.transport());
            client.transportConnected(generation);

            frontend::ProtocolErrorMessage closing;
            closing.code = testCase.code;
            closing.message = "classified closing protocol error";
            closing.closeConnection = true;
            const bool accepted = client.receive(generation, frontend::ServerMessage{std::move(closing)});
            const auto terminal = std::find_if(stateChanges.rbegin(), stateChanges.rend(), [](const core::StateChange& change) {
                return change.current == core::ConnectionState::Disconnected;
            });

            result.expectTrue(accepted && client.connectionState() == core::ConnectionState::Disconnected && protocolMessages == 1 &&
                                  terminal != stateChanges.rend() && terminal->error.has_value() &&
                                  terminal->error->origin == core::ErrorOrigin::Protocol &&
                                  terminal->error->protocolCode == testCase.code && terminal->error->retryable == testCase.retryable,
                              "a semantically accepted closing protocol error preserves its retry classification: " +
                                  std::string(frontend::toString(testCase.code)));
        }
    }

    void testCompatibilityExtensionFallback(tests::support::TestResult& result) {
        Harness harness;
        std::vector<std::string> observedMethods;
        core::ClientCallbacks callbacks;
        callbacks.onStateUpdated = [&observedMethods](const core::StateUpdate& update) {
            for (const core::Change& change : update.changes) {
                const auto* extension = std::get_if<core::CompatibilityExtensionChange>(&change);
                if (extension != nullptr) {
                    const auto method = extension->extensions.json().find("method");
                    if (method != extension->extensions.json().end() && method->is_string()) {
                        observedMethods.push_back(method->get<std::string>());
                    }
                }
            }
        };
        core::ClientOptions options = clientOptions();
        options.allowLegacyV1 = false;
        core::ClientCore client(std::move(options), std::move(callbacks));
        const auto generation = client.attach(harness.transport());
        client.transportConnected(*generation);
        initialReady(client, *generation);
        frontend::FrontendEvent firstExtension{frontend::SequenceNumber(2),
                                               "codex.extension",
                                               frontend::Json{{"method", "vendor/future"}, {"params", frontend::Json{{"safe", true}}}}};
        frontend::FrontendEvent secondExtension{frontend::SequenceNumber(3),
                                                "codex.extension",
                                                frontend::Json{{"method", "vendor/later"}, {"params", frontend::Json{{"safe", false}}}}};
        const bool accepted = client.receive(
            *generation,
            frontend::ServerMessage{frontend::EventBatch{
                firstExtension.sequence, secondExtension.sequence, {std::move(firstExtension), std::move(secondExtension)}}});
        result.expectTrue(accepted && client.ready() && observedMethods == std::vector<std::string>{"vendor/future", "vendor/later"} &&
                              client.state()->visibleSequence == model::FrontendSequence(3) &&
                              client.state()->representation == core::RepresentationMode::ExpandedV1,
                          "multiple bounded codex.extension occurrences retain safe descriptors as observable Changes in one batch");
    }

    void testIndependentEventRepresentations(tests::support::TestResult& result) {
        const std::vector<frontend::FrontendCapability> notifications{
            frontend::FrontendCapability::DedicatedNotificationEvents,
        };
        Harness notificationHarness;
        core::ClientOptions notificationOptions = clientOptions();
        notificationOptions.requestedCapabilities = notifications;
        core::ClientCore notificationClient(std::move(notificationOptions));
        const core::PhysicalGeneration notificationGeneration =
            readyWithCapabilities(notificationClient, notificationHarness, notifications);
        frontend::FrontendEvent expandedDiagnostic = diagnosticEvent(1, true);
        const bool expandedDiagnosticAccepted =
            notificationClient.receive(notificationGeneration,
                                       frontend::ServerMessage{frontend::EventBatch{
                                           expandedDiagnostic.sequence, expandedDiagnostic.sequence, {std::move(expandedDiagnostic)}}});
        frontend::FrontendEvent legacyContent = itemContentEvent(2, false);
        const bool legacyContentAccepted = notificationClient.receive(
            notificationGeneration,
            frontend::ServerMessage{frontend::EventBatch{legacyContent.sequence, legacyContent.sequence, {std::move(legacyContent)}}});

        const std::vector<frontend::FrontendCapability> items{
            frontend::FrontendCapability::CompleteThreadItems,
        };
        Harness itemHarness;
        core::ClientOptions itemOptions = clientOptions();
        itemOptions.requestedCapabilities = items;
        core::ClientCore itemClient(std::move(itemOptions));
        const core::PhysicalGeneration itemGeneration = readyWithCapabilities(itemClient, itemHarness, items);
        frontend::FrontendEvent legacyDiagnostic = diagnosticEvent(1, false);
        const bool legacyDiagnosticAccepted =
            itemClient.receive(itemGeneration,
                               frontend::ServerMessage{frontend::EventBatch{
                                   legacyDiagnostic.sequence, legacyDiagnostic.sequence, {std::move(legacyDiagnostic)}}});
        frontend::FrontendEvent expandedContent = itemContentEvent(2, true);
        const bool expandedContentAccepted =
            itemClient.receive(itemGeneration,
                               frontend::ServerMessage{
                                   frontend::EventBatch{expandedContent.sequence, expandedContent.sequence, {std::move(expandedContent)}}});
        result.expectTrue(expandedDiagnosticAccepted && legacyContentAccepted && notificationClient.ready() && legacyDiagnosticAccepted &&
                              expandedContentAccepted && itemClient.ready() &&
                              notificationClient.state()->representation == core::RepresentationMode::LegacyV1 &&
                              itemClient.state()->representation == core::RepresentationMode::LegacyV1,
                          "overlapping event type strings decode independently from notification and item capability selections");

        Harness mixedHarness;
        core::ClientOptions mixedOptions = clientOptions();
        mixedOptions.requestedCapabilities = notifications;
        core::ClientCore mixedClient(std::move(mixedOptions));
        const core::PhysicalGeneration mixedGeneration = readyWithCapabilities(mixedClient, mixedHarness, notifications);
        frontend::FrontendEvent mixedExpanded = diagnosticEvent(1, true);
        frontend::FrontendEvent mixedLegacy = itemContentEvent(2, false);
        const bool mixedAccepted =
            mixedClient.receive(mixedGeneration,
                                frontend::ServerMessage{frontend::EventBatch{
                                    mixedExpanded.sequence, mixedLegacy.sequence, {std::move(mixedExpanded), std::move(mixedLegacy)}}});
        result.expectTrue(!mixedAccepted && mixedClient.connectionState() == core::ConnectionState::Disconnected,
                          "one EventBatch cannot mix independently negotiated expanded and legacy occurrence groups");
    }

    void testBatchReductionIsTransactional(tests::support::TestResult& result) {
        const std::vector<frontend::FrontendCapability> items{
            frontend::FrontendCapability::CompleteThreadItems,
        };
        Harness harness;
        std::vector<std::string> diagnostics;
        core::ClientCallbacks callbacks;
        callbacks.onDiagnostic = [&diagnostics](const core::Diagnostic& diagnostic) {
            diagnostics.push_back(diagnostic.message);
        };
        core::ClientOptions options = clientOptions();
        options.requestedCapabilities = items;
        core::ClientCore client(std::move(options), std::move(callbacks));
        const core::PhysicalGeneration generation = readyWithCapabilities(client, harness, items);
        const auto before = client.state();

        frontend::FrontendEvent valid = itemContentEvent(1, true);
        model::ItemContentUpdatedOccurrence missing{model::ItemIdentity{"missing-item"}};
        missing.threadId = model::ThreadIdentity{"partial-thread"};
        missing.turnId = model::TurnIdentity{"partial-turn"};
        missing.channel = "commandOutput";
        missing.content = "must not publish";
        frontend::FrontendEvent invalid = occurrenceEvent(2, std::move(missing), true);
        const bool accepted = client.receive(
            generation,
            frontend::ServerMessage{
                frontend::EventBatch{valid.sequence, invalid.sequence, {std::move(valid), std::move(invalid)}}});
        const auto after = client.state();
        const model::ThreadItem* item = after ? after->item("partial-item") : nullptr;
        const bool reductionRejected = std::ranges::any_of(diagnostics, [](const std::string& diagnostic) {
            return diagnostic.find("canonical occurrence reduction rejected an event group") != std::string::npos;
        });
        result.expectTrue(!accepted && client.connectionState() == core::ConnectionState::Disconnected && reductionRejected,
                          "the invalid second occurrence rejects the complete event batch");
        result.expectTrue(before && after && before->snapshot && after->snapshot && *after->snapshot == *before->snapshot,
                          "a rejected event batch retains the prior canonical public State rather than its private candidate");
        result.expectTrue(item != nullptr &&
                              model::itemData(*item).commandOutput == std::optional<std::string>{"initial"},
                          "a rejected event batch does not leak its earlier valid occurrence into public State");
    }

    void testNegotiatedItemContentAppend(tests::support::TestResult& result) {
        const std::vector<frontend::FrontendCapability> items{frontend::FrontendCapability::CompleteThreadItems};
        Harness harness;
        bool observedReplacementChange = false;
        core::ClientCallbacks callbacks;
        callbacks.onStateUpdated = [&observedReplacementChange](const core::StateUpdate& update) {
            observedReplacementChange = observedReplacementChange || std::ranges::any_of(update.changes, [](const core::Change& change) {
                                            const auto* content = std::get_if<model::ItemContentUpdatedOccurrence>(&change);
                                            return content != nullptr && content->appendWireRepresentation;
                                        });
        };
        core::ClientOptions options = clientOptions();
        options.requestedCapabilities = items;
        core::ClientCore client(std::move(options), std::move(callbacks));
        const core::PhysicalGeneration generation = readyWithCapabilities(client, harness, items, true);
        const auto* hello = !harness.outbound.empty() ? std::get_if<frontend::Hello>(&harness.outbound.front().value) : nullptr;
        const frontend::Json* requestedModes = nullptr;
        if (hello != nullptr) {
            const auto projection = hello->extensions.find("projection");
            if (projection != hello->extensions.end() && projection->is_object()) {
                const auto modes = projection->find("itemContentUpdateModes");
                if (modes != projection->end()) {
                    requestedModes = &*modes;
                }
            }
        }
        frontend::FrontendEvent append = itemContentAppendEvent(1, 7, "+delta");
        const bool accepted = client.receive(
            generation,
            frontend::ServerMessage{frontend::EventBatch{append.sequence, append.sequence, {std::move(append)}}});
        const model::ThreadItem* item = client.state() ? client.state()->item("partial-item") : nullptr;
        result.expectTrue(requestedModes != nullptr && *requestedModes == frontend::Json::array({"append-v1"}),
                          "client Hello explicitly offers the append-v1 item-content update mode");
        result.expectTrue(accepted && client.ready() && item != nullptr &&
                              model::itemData(*item).commandOutput == std::optional<std::string>{"initial+delta"} &&
                              observedReplacementChange,
                          "a Welcome-selected append-v1 update reduces canonically and remains an item-content replacement change");

        Harness legacyHarness;
        core::ClientOptions legacyOptions = clientOptions();
        legacyOptions.requestedCapabilities = items;
        core::ClientCore legacy(std::move(legacyOptions));
        const core::PhysicalGeneration legacyGeneration = readyWithCapabilities(legacy, legacyHarness, items);
        frontend::FrontendEvent unnegotiated = itemContentAppendEvent(1, 7, "+delta");
        const bool unnegotiatedAccepted = legacy.receive(
            legacyGeneration,
            frontend::ServerMessage{
                frontend::EventBatch{unnegotiated.sequence, unnegotiated.sequence, {std::move(unnegotiated)}}});
        result.expectTrue(!unnegotiatedAccepted && legacy.connectionState() == core::ConnectionState::Disconnected,
                          "append-v1 wire data is rejected unless the Welcome selected the exact offered mode");

        Harness invalidHarness;
        core::ClientOptions invalidOptions = clientOptions();
        invalidOptions.requestedCapabilities = items;
        core::ClientCore invalid(std::move(invalidOptions));
        const core::PhysicalGeneration invalidGeneration = *invalid.attach(invalidHarness.transport());
        invalid.transportConnected(invalidGeneration);
        frontend::Welcome invalidWelcome{"invalid-append-session",
                                         frontend::SessionRole::Observer,
                                         frontend::SequenceNumber{0},
                                         frontend::SyncMode::Snapshot,
                                         {{"permittedScopes", frontend::Json::array({"observe"})},
                                          {"projection", {{"itemContentUpdateMode", "future-mode"}}}},
                                         representationCapabilities(items),
                                         allMethods(),
                                         allMethods()};
        const bool invalidAccepted = invalid.receive(invalidGeneration, frontend::ServerMessage{std::move(invalidWelcome)});
        result.expectTrue(!invalidAccepted && invalid.connectionState() == core::ConnectionState::Disconnected,
                          "an unoffered item-content update mode in Welcome is a protocol error");
    }

    void testLegacyUnknownCompatibility(tests::support::TestResult& result) {
        Harness harness;
        core::ClientOptions options = clientOptions();
        options.requestedCapabilities.clear();
        core::ClientCore client(std::move(options));
        const core::PhysicalGeneration generation = readyWithCapabilities(client, harness, {});

        model::CanonicalSnapshot canonical = itemSnapshotState(1);
        model::LegacyItemCompatibility future{
            model::ItemData{model::ItemIdentity{"future-item"},
                            model::ThreadIdentity{"partial-thread"},
                            model::TurnIdentity{"partial-turn"}},
            "future_codex_item_kind",
            1,
            "/threads/0/turns/0/items/1"};
        future.value.sourceIndex = 1;
        future.value.status = "completed";
        future.value.safeDetails = *model::SafeDetail::fromJson(frontend::Json{{"safeSentinel", "future-safe"}});
        canonical.legacyItems.push_back(std::move(future));
        model::LegacyPendingRequestCompatibility pending{
            model::PendingRequestData{model::PendingRequestIdentity{"72"}}, 0, "/pendingRequests/0"};
        pending.value.sourceIndex = 0;
        pending.value.safeDetails =
            *model::SafeDetail::fromJson(frontend::Json{{"method", "future/serverRequest"}, {"sensitiveFieldsRedacted", true}});
        canonical.legacyPendingRequests.push_back(std::move(pending));
        const auto wire = model::encodeProjectedSnapshot(canonical, model::SnapshotRepresentationSelection{});
        const bool accepted = wire && client.receive(generation, frontend::ServerMessage{wire.value()});
        frontend::FrontendEvent liveItem{
            frontend::SequenceNumber{2},
            "item.updated",
            {{"threadId", "partial-thread"},
             {"turnId", "partial-turn"},
             {"item",
              {{"id", "future-item"},
               {"type", "future_codex_item_kind"},
               {"status", "completed"},
               {"agentText", "updated"},
               {"reasoningText", ""},
               {"reasoningSummary", ""},
               {"commandOutput", ""},
               {"droppedContentBytes", 0},
               {"contentTruncated", false},
               {"data", {{"safeSentinel", "future-live-safe"}}},
               {"extensions", frontend::Json::object()}}}}};
        frontend::FrontendEvent livePending{
            frontend::SequenceNumber{3},
            "request.pending",
            {{"request",
              {{"id", "73"},
               {"type", "unknown"},
               {"details", {{"method", "future/liveRequest"}, {"sensitiveFieldsRedacted", true}}}}}}};
        const bool liveAccepted =
            client.receive(generation,
                           frontend::ServerMessage{frontend::EventBatch{liveItem.sequence, liveItem.sequence, {std::move(liveItem)}}}) &&
            client.receive(generation,
                           frontend::ServerMessage{frontend::EventBatch{
                               livePending.sequence, livePending.sequence, {std::move(livePending)}}});
        const auto published = client.state();
        const frontend::Json serialized = published ? published->serializeForTesting() : frontend::Json::object();
        result.expectTrue(accepted && liveAccepted && client.ready() && published && published->snapshot->legacyItems.size() == 1 &&
                              published->snapshot->legacyItems.front().discriminator == "future_codex_item_kind" &&
                              published->snapshot->legacyItems.front().value.agentText == std::optional<std::string>{"updated"} &&
                              published->snapshot->legacyPendingRequests.size() == 2 &&
                              serialized.dump().find("future_codex_item_kind") != std::string::npos &&
                              serialized.dump().find("future/serverRequest") != std::string::npos &&
                              serialized.dump().find("future/liveRequest") != std::string::npos,
                          "legacy Snapshot and live reduction retain bounded future items and generic pending requests without expanding closed enums");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testSnapshotReplayAndProjectionRefresh(result);
    testLegacyAndExplicitOrdering(result);
    testSynchronizationPublicationsAndGuards(result);
    testProjectedSequenceRules(result);
    testLiveSnapshotCursorRules(result);
    testLiveCallbackLifecycleInvalidation(result);
    testClosingProtocolErrorAcceptance(result);
    testCompatibilityExtensionFallback(result);
    testIndependentEventRepresentations(result);
    testBatchReductionIsTransactional(result);
    testNegotiatedItemContentAppend(result);
    testLegacyUnknownCompatibility(result);
    return result.processResult();
}
