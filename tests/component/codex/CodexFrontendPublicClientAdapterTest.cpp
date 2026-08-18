/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/client/Client.h"
#include "ai/openai/codex/frontend/client/detail/ClientTestAccess.h"
#include "ai/openai/codex/frontend/internal/client/CanonicalStateBuilder.h"
#include "ai/openai/codex/frontend/internal/client/ClientCore.h"
#include "ai/openai/codex/frontend/internal/model/Model.h"
#include "ai/openai/codex/frontend/internal/model/Occurrence.h"
#include "support/TestResult.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace client = frontend::client;
    namespace core = frontend::internal::client;
    namespace model = frontend::internal::model;
    namespace generated = frontend::generated;
    namespace typed = ai::openai::codex::typed;

    std::string traceText(const std::vector<std::string>& trace) {
        std::string result;
        for (const std::string& entry : trace) {
            if (!result.empty()) {
                result += ',';
            }
            result += entry;
        }
        return result;
    }

    std::vector<frontend::FrontendMethod> allMethods() {
        std::vector<frontend::FrontendMethod> result;
        result.reserve(generated::AllMethods.size());
        for (const generated::MethodMetadata& method : generated::AllMethods) {
            result.emplace_back(method.method);
        }
        return result;
    }

    frontend::CapabilityAdvertisement expandedCapabilities() {
        std::vector<frontend::FrontendCapability> defined;
        for (const generated::CapabilityMetadata& capability : generated::AllCapabilities) {
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
        return {std::move(defined), selected, selected, frontend::Json::object()};
    }

    std::optional<client::State> buildCanonicalState(client::Client& adopter,
                                                     const core::PublishedState& publication,
                                                     std::size_t maximumBytes,
                                                     std::size_t maximumRetainedDiagnostics,
                                                     std::string& error,
                                                     client::detail::CanonicalStateBuildFailure* failure = nullptr) {
        auto storage = client::detail::CanonicalStateBuilder::build(publication, maximumBytes, maximumRetainedDiagnostics, error, failure);
        if (!storage.has_value()) {
            return std::nullopt;
        }
        return client::detail::ClientTestAccess::adoptStateStorage(adopter, std::move(*storage));
    }

    frontend::Welcome welcome(std::uint64_t sequence) {
        return {"7",
                frontend::SessionRole::Observer,
                frontend::SequenceNumber(sequence),
                frontend::SyncMode::Snapshot,
                frontend::Json{{"permittedScopes", frontend::Json::array({"observe", "control"})},
                               {"projection", frontend::Json{{"identity", "public-adapter"}}}},
                expandedCapabilities(),
                allMethods(),
                allMethods(),
                std::nullopt,
                768U * 1024U};
    }

    model::CanonicalSnapshot canonicalSnapshot(std::uint64_t sequence, std::uint64_t generation, std::string title) {
        model::CanonicalSnapshot snapshot;
        snapshot.sequence = model::FrontendSequence(sequence);
        snapshot.provider.lifecycle = model::ProviderLifecycle::Ready;
        snapshot.provider.generation = generation;
        snapshot.provider.desiredRunning = true;
        snapshot.sessions.emplace_back(model::SessionIdentity{"7"});
        snapshot.threads.emplace_back(model::ThreadIdentity{"adapter-thread"});
        snapshot.threads.back().title = std::move(title);
        snapshot.threadList.hasLoadedPage = true;
        snapshot.threadList.complete = true;
        snapshot.threadList.pagesLoaded = 1;
        snapshot.backendCursor.currentSequence = snapshot.sequence;
        return snapshot;
    }

    frontend::Snapshot expandedSnapshot(std::uint64_t sequence, std::uint64_t generation, std::string title) {
        const auto expanded = model::encodeSnapshot(canonicalSnapshot(sequence, generation, std::move(title)));
        if (!expanded) {
            return {frontend::SequenceNumber(sequence), frontend::Json{{"invalid", true}, {"error", expanded.error().message}}};
        }
        const auto encoded = frontend::Codec::encodeExpandedSnapshot(expanded.value());
        return {frontend::SequenceNumber(sequence),
                encoded ? encoded.value().at("state") : frontend::Json{{"invalid", true}, {"error", encoded.error().message}}};
    }

    frontend::Snapshot expandedUserMessageSnapshot(std::uint64_t sequence, std::uint64_t generation, std::string title) {
        model::CanonicalSnapshot snapshot = canonicalSnapshot(sequence, generation, std::move(title));
        snapshot.turns.emplace_back(model::TurnIdentity{"runtime-turn"}, model::ThreadIdentity{"adapter-thread"});
        const std::string sourceText = std::string(16'383, 'm') + "€" + std::string(16'381, 'n');
        const frontend::Json sourceContent =
            frontend::Json::array({frontend::Json{{"type", "text"}, {"text", sourceText}}});
        model::ItemData item{
            model::ItemIdentity{"runtime-user-item"}, model::ThreadIdentity{"adapter-thread"}, model::TurnIdentity{"runtime-turn"}};
        item.safeDetails = *model::SafeDetail::fromJson(frontend::Json{{"clientId", nullptr},
                                                                      {"content", sourceContent},
                                                                      {"contentTruncated", false},
                                                                      {"originalContentBytes", sourceContent.dump().size()},
                                                                      {"retainedContentBytes", sourceContent.dump().size()},
                                                                      {"originalContentItems", 1},
                                                                      {"retainedContentItems", 1}});
        snapshot.items.push_back(model::UserMessageItem{std::move(item)});
        const auto expanded = model::encodeSnapshot(snapshot);
        if (!expanded) {
            return {frontend::SequenceNumber(sequence), frontend::Json{{"invalid", true}, {"error", expanded.error().message}}};
        }
        const auto encoded = frontend::Codec::encodeExpandedSnapshot(expanded.value());
        return {frontend::SequenceNumber(sequence),
                encoded ? encoded.value().at("state") : frontend::Json{{"invalid", true}, {"error", encoded.error().message}}};
    }

    frontend::Snapshot expandedScopedItemsSnapshot(std::uint64_t sequence) {
        model::CanonicalSnapshot snapshot = canonicalSnapshot(sequence, 3, "scoped items");
        snapshot.threads.emplace_back(model::ThreadIdentity{"second-thread"});
        snapshot.turns.emplace_back(model::TurnIdentity{"first-turn"}, model::ThreadIdentity{"adapter-thread"});
        snapshot.turns.emplace_back(model::TurnIdentity{"second-turn"}, model::ThreadIdentity{"second-thread"});

        model::ItemData first{
            model::ItemIdentity{"item-1"}, model::ThreadIdentity{"adapter-thread"}, model::TurnIdentity{"first-turn"}};
        first.agentText = "first scoped item";
        snapshot.items.push_back(model::AgentMessageItem{std::move(first)});
        model::ItemData second{
            model::ItemIdentity{"item-1"}, model::ThreadIdentity{"second-thread"}, model::TurnIdentity{"second-turn"}};
        second.agentText = "second scoped item";
        snapshot.items.push_back(model::AgentMessageItem{std::move(second)});

        const auto expanded = model::encodeSnapshot(snapshot);
        if (!expanded) {
            return {frontend::SequenceNumber(sequence), frontend::Json{{"invalid", true}, {"error", expanded.error().message}}};
        }
        const auto encoded = frontend::Codec::encodeExpandedSnapshot(expanded.value());
        return {frontend::SequenceNumber(sequence),
                encoded ? encoded.value().at("state") : frontend::Json{{"invalid", true}, {"error", encoded.error().message}}};
    }

    frontend::FrontendEvent occurrenceEvent(model::OccurrenceIdentity identity, model::OccurrencePayload payload) {
        auto occurrence = model::makeOccurrence(std::move(identity), std::move(payload));
        if (!occurrence) {
            return {frontend::SequenceNumber{}, "invalid", frontend::Json::object()};
        }
        auto expanded = model::encodeExpandedOccurrence(occurrence.value());
        if (!expanded || expanded.value().empty()) {
            return {frontend::SequenceNumber{}, "invalid", frontend::Json::object()};
        }
        const frontend::ExpandedFrontendEvent& value = expanded.value().front();
        return {value.sequence, std::string(frontend::toString(value.type)), value.data, value.extensions};
    }

    frontend::FrontendEvent scopedItemUpsertEvent(std::uint64_t sequence) {
        model::ItemData replacement{
            model::ItemIdentity{"item-1"}, model::ThreadIdentity{"second-thread"}, model::TurnIdentity{"second-turn"}};
        replacement.agentText = "second upserted item";
        model::OccurrenceIdentity identity{model::FrontendSequence(sequence),
                                           model::OccurrenceGroupIdentity{"scoped-upsert"},
                                           0,
                                           1,
                                           model::SourceStamp{"adapter-source"}};
        identity.threadId = replacement.threadId;
        identity.turnId = replacement.turnId;
        identity.itemId = replacement.id;
        return occurrenceEvent(
            std::move(identity), model::ItemUpsertedOccurrence{model::AgentMessageItem{std::move(replacement)}});
    }

    frontend::FrontendEvent scopedItemContentEvent(std::uint64_t sequence) {
        model::ItemContentUpdatedOccurrence update{model::ItemIdentity{"item-1"}};
        update.threadId = model::ThreadIdentity{"second-thread"};
        update.turnId = model::TurnIdentity{"second-turn"};
        update.channel = "agentText";
        update.content = "second streamed item";
        model::OccurrenceIdentity identity{model::FrontendSequence(sequence),
                                           model::OccurrenceGroupIdentity{"scoped-content"},
                                           0,
                                           1,
                                           model::SourceStamp{"adapter-source"}};
        identity.threadId = update.threadId;
        identity.turnId = update.turnId;
        identity.itemId = update.itemId;
        return occurrenceEvent(std::move(identity), std::move(update));
    }

    frontend::FrontendEvent providerEvent(std::uint64_t sequence, std::uint64_t generation) {
        model::ProviderState provider;
        provider.lifecycle = model::ProviderLifecycle::Ready;
        provider.generation = generation;
        provider.desiredRunning = true;
        model::OccurrenceIdentity identity{
            model::FrontendSequence(sequence), model::OccurrenceGroupIdentity{"adapter-live"}, 0, 1, model::SourceStamp{"adapter-source"}};
        auto occurrence = model::makeOccurrence(std::move(identity), model::ProviderUpdatedOccurrence{std::move(provider)});
        if (!occurrence) {
            return {frontend::SequenceNumber(sequence), "invalid", frontend::Json::object()};
        }
        auto expanded = model::encodeExpandedOccurrence(occurrence.value());
        if (!expanded || expanded.value().empty()) {
            return {frontend::SequenceNumber(sequence), "invalid", frontend::Json::object()};
        }
        const frontend::ExpandedFrontendEvent& value = expanded.value().front();
        return {value.sequence, std::string(frontend::toString(value.type)), value.data, value.extensions};
    }

    client::ClientOptions publicOptions() {
        client::ClientOptions result;
        result.credentialProvider = [] {
            return client::AuthenticationContext{frontend::NoCredential{}, std::string{"adapter-continuity"}};
        };
        return result;
    }

    struct PublicHarness {
        std::vector<client::OutboundMessage> outbound;
        std::vector<std::string> callbackOrder;
        std::vector<std::uint64_t> updateRevisions;
        std::vector<std::uint64_t> synchronizedRevisions;
        std::vector<frontend::SequenceNumber> cursors;
        std::vector<std::string> diagnostics;
        std::vector<client::Error> connectionErrors;
        std::vector<client::Change> changes;
        std::size_t protocolMessages = 0;
        std::size_t closes = 0;
        bool recording = false;
        bool revisionMismatch = false;
        bool readySawCommittedState = false;
        client::Client* sdk = nullptr;

        client::TransportCallbacks transport() {
            return {[this](client::OutboundMessage message) {
                        outbound.push_back(std::move(message));
                        return client::SendResult{client::SendStatus::Accepted, std::nullopt};
                    },
                    [this](std::string) {
                        ++closes;
                    }};
        }

        client::ClientCallbacks callbacks() {
            client::ClientCallbacks result;
            result.onConnectionStateChanged = [this](const client::ConnectionStateChange& change) {
                if (change.error.has_value()) {
                    diagnostics.push_back(change.error->message);
                    connectionErrors.push_back(*change.error);
                }
                if (recording && change.current == client::ConnectionState::Ready) {
                    callbackOrder.emplace_back("ready");
                    readySawCommittedState = sdk != nullptr && sdk->state().freshness() == client::StateFreshness::Current &&
                                             sdk->state().visibleSequence() == frontend::SequenceNumber(7);
                }
            };
            result.onStateUpdated = [this](const client::StateUpdate& update) {
                if (sdk != nullptr && sdk->state().revision() != update.state.revision()) {
                    revisionMismatch = true;
                }
                updateRevisions.push_back(update.state.revision());
                changes.insert(changes.end(), update.changes.begin(), update.changes.end());
                if (recording) {
                    callbackOrder.emplace_back("state");
                }
            };
            result.onSynchronized = [this](const client::SynchronizationInfo& info) {
                if (sdk != nullptr && sdk->state().revision() != info.state.revision()) {
                    revisionMismatch = true;
                }
                synchronizedRevisions.push_back(info.state.revision());
                if (recording) {
                    callbackOrder.emplace_back("synchronized");
                }
            };
            result.onCursorAdvanced = [this](frontend::SequenceNumber sequence) {
                cursors.push_back(sequence);
                if (recording) {
                    callbackOrder.emplace_back("cursor");
                }
            };
            result.onProtocolMessage = [this](const frontend::ServerMessage&) {
                ++protocolMessages;
                if (recording) {
                    callbackOrder.emplace_back("protocol");
                }
            };
            result.onDiagnostic = [this](const client::Diagnostic& diagnostic) {
                diagnostics.push_back(diagnostic.message);
            };
            return result;
        }
    };

    template <typename Change>
    const Change* findChange(const std::vector<client::Change>& changes) {
        for (const client::Change& change : changes) {
            if (const auto* value = std::get_if<Change>(&change)) {
                return value;
            }
        }
        return nullptr;
    }

    void testDirectCanonicalStateBuilder(tests::support::TestResult& result) {
        client::Client adopter(publicOptions());
        core::PublishedState publication;
        publication.revision = 41;
        publication.freshness = core::PublishedFreshness::Current;
        publication.representation = core::RepresentationMode::ExpandedV1;
        publication.visibleSequence = model::FrontendSequence{17};
        publication.synchronizedThrough = model::FrontendSequence{16};
        publication.projectionFingerprint = "direct-canonical-fingerprint";
        publication.session.emplace(model::SessionIdentity{"7"});
        publication.session->synchronizationMode = frontend::SyncMode::Snapshot;
        model::CanonicalSnapshot direct = canonicalSnapshot(17, 3, "first title");
        direct.reviews.state = model::DomainState::present();
        direct.integrations.state = model::DomainState::present();
        direct.plugins.state = model::DomainState::present();
        direct.platform.state = model::DomainState::present();
        direct.threads.front().safeDetails =
            *model::SafeDetail::fromJson(frontend::Json{{"status", "idle"},
                                                        {"realtime",
                                                         {{"lifecycle", "failed"},
                                                          {"transcript", "hello"},
                                                          {"lastError", "safe realtime error"},
                                                          {"errorDetailsOmitted", false},
                                                          {"itemCount", 2},
                                                          {"receivedAudioBytes", 3},
                                                          {"droppedAudioBytes", 4},
                                                          {"transcriptTruncated", false},
                                                          {"sourceGeneration", 5},
                                                          {"sourceFreshness", "current"}}}});
        model::TurnState semanticTurn{model::TurnIdentity{"semantic-turn"}, model::ThreadIdentity{"adapter-thread"}};
        semanticTurn.status = "failed";
        semanticTurn.safeDetails = *model::SafeDetail::fromJson(frontend::Json{
            {"tokenUsage", {{"modelContextWindow", nullptr}}},
            {"tokenUsageLast",
             {{"cachedInputTokens", 1}, {"inputTokens", 2}, {"outputTokens", 3}, {"reasoningOutputTokens", 4}, {"totalTokens", 5}}},
            {"tokenUsageTotal",
             {{"cachedInputTokens", 6}, {"inputTokens", 7}, {"outputTokens", 8}, {"reasoningOutputTokens", 9}, {"totalTokens", 10}}},
            {"tokenUsageModelContextWindow", nullptr},
            {"tokenUsageModelContextWindowPresent", true},
            {"failure", {{"message", "safe turn error"}, {"additionalDetails", "display details"}}},
            {"failureMessage", "safe turn error"},
            {"failureAdditionalDetails", "display details"},
            {"failureAdditionalDetailsPresent", true},
            {"failureCodexErrorInfoPresent", true},
            {"failureCodexErrorDiscriminator", "activeTurnNotSteerable"},
            {"failureNonSteerableTurnKind", "review"}});
        direct.turns.push_back(std::move(semanticTurn));
        model::ItemData semanticItem{
            model::ItemIdentity{"semantic-item"}, model::ThreadIdentity{"adapter-thread"}, model::TurnIdentity{"semantic-turn"}};
        semanticItem.safeDetails = *model::SafeDetail::fromJson(frontend::Json{{"command", "cmake --build"},
                                                                               {"cwd", "/workspace"},
                                                                               {"status", "completed"},
                                                                               {"processId", "42"},
                                                                               {"exitCode", 0},
                                                                               {"durationMs", 13}});
        semanticItem.generation = 6;
        semanticItem.freshness = model::Freshness::Current;
        direct.items.push_back(model::CommandExecutionItem{std::move(semanticItem)});
        const std::string semanticUserText = "hello\n\nGrüße";
        model::ItemData semanticUserItem{
            model::ItemIdentity{"semantic-user-item"}, model::ThreadIdentity{"adapter-thread"}, model::TurnIdentity{"semantic-turn"}};
        semanticUserItem.safeDetails = *model::SafeDetail::fromJson(frontend::Json{{"clientId", "client-user-1"},
                                                                                   {"contentTruncated", false},
                                                                                   {"text", semanticUserText},
                                                                                   {"textTruncated", false},
                                                                                   {"originalContentBytes", 128},
                                                                                   {"retainedContentBytes", 128},
                                                                                   {"originalContentItems", 3},
                                                                                   {"retainedContentItems", 3}});
        direct.items.push_back(model::UserMessageItem{std::move(semanticUserItem)});
        direct.controller.safeDetails = *model::SafeDetail::fromJson(frontend::Json{{"present", true}});
        direct.truncation.extensions = *model::SafeDetail::fromJson(frontend::Json{{"vendorTruncation", "state-truncation-extension"}});
        direct.processesState.extensions = *model::SafeDetail::fromJson(frontend::Json{{"vendorCollection", "processes-extension"}});
        direct.processesState.truncation.extensions =
            *model::SafeDetail::fromJson(frontend::Json{{"vendorTruncation", "processes-extension-truncation"}});
        model::ProcessState process{model::ProcessHandle{"adapter-process"}};
        process.stamp.extensions = *model::SafeDetail::fromJson(frontend::Json{{"vendorStamp", "process-stamp-extension"}});
        direct.processes.push_back(std::move(process));
        model::PendingRequestData emptyQuestions{model::PendingRequestIdentity{"adapter-pending"}};
        emptyQuestions.questionsPresent = true;
        direct.pendingRequests.push_back(model::UserInputRequest{std::move(emptyQuestions)});
        model::PendingRequestData approval{model::PendingRequestIdentity{"semantic-pending"}};
        approval.safeDetails = *model::SafeDetail::fromJson(
            frontend::Json{{"commandBytes", 20}, {"commandRedacted", true}, {"reasonBytes", 12}, {"reasonRedacted", true}});
        direct.pendingRequests.push_back(model::CommandExecutionApprovalRequest{std::move(approval)});
        direct.extensions = *model::SafeDetail::fromJson(frontend::Json{{"providerOperationsSemantic",
                                                                         {{"methods", frontend::Json::array({"thread/goal/set"})},
                                                                          {"resultAlternatives", frontend::Json::array({17})},
                                                                          {"generations", frontend::Json::array({8})},
                                                                          {"freshness", frontend::Json::array({"current"})},
                                                                          {"truncated", false},
                                                                          {"omittedEntries", 0}}},
                                                                        {"conversationSemantic",
                                                                         {{"resultMethods", frontend::Json::array()},
                                                                          {"resultAlternatives", frontend::Json::array()},
                                                                          {"resultStatuses", frontend::Json::array()},
                                                                          {"resultSubjectIds", frontend::Json::array()},
                                                                          {"resultSubjectIdPresent", frontend::Json::array()},
                                                                          {"resultNextCursors", frontend::Json::array()},
                                                                          {"resultNextCursorPresent", frontend::Json::array()},
                                                                          {"resultItemCounts", frontend::Json::array()},
                                                                          {"resultComplete", frontend::Json::array()},
                                                                          {"resultGenerations", frontend::Json::array()},
                                                                          {"resultFreshness", frontend::Json::array()},
                                                                          {"notificationMethods", frontend::Json::array()},
                                                                          {"notificationAlternatives", frontend::Json::array()},
                                                                          {"notificationGenerations", frontend::Json::array()},
                                                                          {"notificationFreshness", frontend::Json::array()},
                                                                          {"omittedResults", 0},
                                                                          {"omittedNotifications", 0},
                                                                          {"truncated", false},
                                                                          {"latestGoalSetOperation", "set"},
                                                                          {"latestGoalSetThreadId", "adapter-thread"},
                                                                          {"latestGoalSetObjective", "finish projection"},
                                                                          {"latestGoalSetStatus", "active"},
                                                                          {"latestGoalSetCleared", nullptr},
                                                                          {"latestGoalSetGeneration", 9},
                                                                          {"latestGoalSetFreshness", "current"}}},
                                                                        {"filesystemProviderSemantic",
                                                                         {{"resultMethods", frontend::Json::array({"fs/readFile"})},
                                                                          {"resultAlternatives", frontend::Json::array({31})},
                                                                          {"resultStatuses", frontend::Json::array({"completed"})},
                                                                          {"resultSubjectIds", frontend::Json::array({""})},
                                                                          {"resultSubjectIdPresent", frontend::Json::array({false})},
                                                                          {"resultNextCursors", frontend::Json::array({""})},
                                                                          {"resultNextCursorPresent", frontend::Json::array({false})},
                                                                          {"resultItemCounts", frontend::Json::array({1})},
                                                                          {"resultComplete", frontend::Json::array({true})},
                                                                          {"resultGenerations", frontend::Json::array({10})},
                                                                          {"resultFreshness", frontend::Json::array({"stale"})},
                                                                          {"notificationMethods", frontend::Json::array()},
                                                                          {"notificationAlternatives", frontend::Json::array()},
                                                                          {"notificationGenerations", frontend::Json::array()},
                                                                          {"notificationFreshness", frontend::Json::array()},
                                                                          {"omittedResults", 0},
                                                                          {"omittedNotifications", 0},
                                                                          {"truncated", false}}},
                                                                        {"capacityProvenance",
                                                                         {{"rejectedSessions", std::numeric_limits<std::uint64_t>::max()},
                                                                          {"rejectedObservers", 12},
                                                                          {"rejectedOperations", 13},
                                                                          {"providerRequestOverflows", 14},
                                                                          {"evictedThreads", 15},
                                                                          {"evictedTurns", 16},
                                                                          {"evictedItems", 17},
                                                                          {"droppedContentBytes", 18},
                                                                          {"snapshotOmissions", 19},
                                                                          {"evictedNotices", 20},
                                                                          {"evictedProcesses", 21},
                                                                          {"droppedProcessOutputBytes", 22},
                                                                          {"evictedFilesystemWatches", 23},
                                                                          {"evictedFuzzySearchSessions", 24},
                                                                          {"evictedActivityRecords", 25},
                                                                          {"maxSessions", 101},
                                                                          {"maxObservers", 102},
                                                                          {"maxActiveOperations", 103},
                                                                          {"maxPendingRequests", 104},
                                                                          {"maxRetainedThreads", 105},
                                                                          {"maxRetainedTurns", 106},
                                                                          {"maxRetainedItems", 107},
                                                                          {"maxAccumulatedContentBytes", 108},
                                                                          {"maxSnapshotBytes", 109},
                                                                          {"maxRetainedNotices", 110},
                                                                          {"maxRetainedProcesses", 111},
                                                                          {"maxProcessOutputBytesPerProcess", 112},
                                                                          {"maxAccumulatedProcessOutputBytes", 113},
                                                                          {"maxRetainedFilesystemWatches", 114},
                                                                          {"maxRetainedFuzzySearchSessions", 115},
                                                                          {"maxRetainedActivityRecords", 116},
                                                                          {"sourceSessionCount", 2},
                                                                          {"sourcePendingRequestCount", 3},
                                                                          {"omittedThreads", 4},
                                                                          {"omittedTurns", 5},
                                                                          {"omittedItems", 6},
                                                                          {"truncated", true},
                                                                          {"mandatoryCoreExceedsLimit", false}}}});
        publication.snapshot = std::make_shared<const model::CanonicalSnapshot>(std::move(direct));

        // This fixture spans every public State section. Keep its compact logical
        // encoding boundary fixed while the production ledger is assembled
        // section-by-section; debug builds also compare it with encodeState().
        constexpr std::size_t exactStateBytes = 8521;
        std::string error;
        const auto first = buildCanonicalState(adopter, publication, exactStateBytes, 64, error);
        std::string belowBoundaryError;
        client::detail::CanonicalStateBuildFailure belowBoundaryFailure =
            client::detail::CanonicalStateBuildFailure::StateDivergence;
        const auto belowBoundary = buildCanonicalState(
            adopter, publication, exactStateBytes - 1, 64, belowBoundaryError, &belowBoundaryFailure);
        const client::ThreadState* firstThread = first ? first->thread("adapter-thread") : nullptr;
        client::ThreadState activeThread;
        client::ThreadState notLoadedThread;
        client::ThreadState systemErrorThread;
        client::ThreadState missingStatusThread;
        client::ThreadState futureStatusThread;
        if (firstThread != nullptr) {
            activeThread = *firstThread;
            activeThread.status = "active";
            notLoadedThread = *firstThread;
            notLoadedThread.status = "notLoaded";
            systemErrorThread = *firstThread;
            systemErrorThread.status = "systemError";
            missingStatusThread = *firstThread;
            missingStatusThread.status.reset();
            futureStatusThread = *firstThread;
            futureStatusThread.status = "futureStatus";
        }
        result.expectTrue(
            first.has_value() && error.empty() && !belowBoundary.has_value() && !belowBoundaryError.empty() &&
                belowBoundaryFailure == client::detail::CanonicalStateBuildFailure::Capacity && first->revision() == 41 &&
                first->freshness() == client::StateFreshness::Current &&
                first->representationMode() == client::RepresentationMode::ExpandedV1 &&
                first->visibleSequence() == frontend::SequenceNumber(17) && first->synchronizedThrough() == frontend::SequenceNumber(16) &&
                first->provider().value.has_value() && first->provider().value->generation == 3 && first->controller().value.has_value() &&
                first->controller().value->present &&
                first->controller().value->extensions.find("present") == first->controller().value->extensions.end() &&
                firstThread != nullptr && firstThread->title == std::optional<std::string>{"first title"} &&
                !first->permissionProfiles().value.has_value() && !first->apps().value.has_value() &&
                !first->externalAgents().value.has_value() && !first->hooks().value.has_value() &&
                !first->marketplace().value.has_value() && !first->skills().value.has_value() &&
                !first->windowsSandbox().value.has_value() && !first->platform().value.has_value() && first->truncation().value &&
                first->truncation().value->extensions.value("vendorTruncation", "") == "state-truncation-extension" &&
                first->processes().value && first->processes().value->extensions.value("vendorCollection", "") == "processes-extension" &&
                first->processes().value->truncation.extensions.value("vendorTruncation", "") == "processes-extension-truncation" &&
                first->processes().value->entries.front().stamp.extensions.value("vendorStamp", "") == "process-stamp-extension" &&
                first->pendingRequests().size() == 2 && first->pendingRequests().front().questions.has_value() &&
                first->pendingRequests().front().questions->empty(),
            "CanonicalStateBuilder maps every sampled public State border and enforces its exact encoded byte boundary");
        result.expectTrue(firstThread != nullptr && client::threadIsIdle(*firstThread) && !client::threadIsIdle(activeThread) &&
                              !client::threadIsIdle(notLoadedThread) && !client::threadIsIdle(systemErrorThread) &&
                              !client::threadIsIdle(missingStatusThread) && !client::threadIsIdle(futureStatusThread),
                          "the typed thread-idle predicate recognizes only the canonical idle status and fails closed otherwise");

        const client::TurnState* publicTurn = first ? first->turn("semantic-turn") : nullptr;
        const client::ItemState* publicItem = first ? first->item("semantic-item") : nullptr;
        const client::ItemState* publicUserItem = first ? first->item("semantic-user-item") : nullptr;
        const client::PendingRequestState* publicPending =
            first ? first->pendingRequest(client::PendingRequestId{"semantic-pending"}) : nullptr;
        const auto usage = publicTurn ? client::tokenUsageView(*publicTurn) : std::nullopt;
        const auto failure = publicTurn ? client::failureView(*publicTurn) : std::nullopt;
        const auto realtime =
            firstThread && firstThread->realtime ? std::optional{client::realtimeSemanticView(*firstThread->realtime)} : std::nullopt;
        const auto itemView = publicItem ? client::itemSemanticView(*publicItem) : std::nullopt;
        const auto userMessageView = publicUserItem ? client::userMessageSemanticView(*publicUserItem) : std::nullopt;
        const auto commandAsUserMessage = publicItem ? client::userMessageSemanticView(*publicItem) : std::nullopt;
        const auto pendingView = publicPending ? std::optional{client::pendingRequestPresentation(*publicPending)} : std::nullopt;
        const auto operations = first ? first->providerOperations() : std::nullopt;
        const auto conversations = first ? first->conversations() : std::nullopt;
        const auto filesystem = first ? first->filesystemProvider() : std::nullopt;
        const auto provenance = first ? first->capacityProvenance() : std::nullopt;
        const auto* command = itemView ? std::get_if<client::CommandExecutionSemanticView>(&itemView->details) : nullptr;
        result.expectTrue(
            usage && usage->last && usage->total && usage->last->reasoningOutputTokens == 4 && usage->total->cachedInputTokens == 6 &&
                usage->modelContextWindowPresent && !usage->modelContextWindow && failure && failure->message == "safe turn error" &&
                failure->codexErrorCategory == "activeTurnNotSteerable" && failure->nonSteerableTurnKind == "review" && realtime &&
                realtime->lastError == "safe realtime error" && realtime->stamp && realtime->stamp->generation == 5 && command &&
                command->command == "cmake --build" && command->durationMs == 13 && pendingView && pendingView->commandBytes == 20 &&
                pendingView->commandRedacted && operations && operations->entries.size() == 1 &&
                operations->entries.front().resultAlternative == 17 && conversations && conversations->latestGoalSet &&
                conversations->latestGoalSet->objective == "finish projection" && filesystem && filesystem->latestResults.size() == 1 &&
                filesystem->latestResults.front().resultAlternative == 31 && provenance && provenance->sourceSessionCount == 2 &&
                provenance->omittedItems == 6 && provenance->truncated &&
                provenance->rejectedSessions == std::numeric_limits<std::uint64_t>::max() &&
                provenance->rejectedObservers == 12 && provenance->rejectedOperations == 13 &&
                provenance->providerRequestOverflows == 14 && provenance->evictedThreads == 15 && provenance->evictedTurns == 16 &&
                provenance->evictedItems == 17 && provenance->droppedContentBytes == 18 && provenance->snapshotOmissions == 19 &&
                provenance->evictedNotices == 20 && provenance->evictedProcesses == 21 &&
                provenance->droppedProcessOutputBytes == 22 && provenance->evictedFilesystemWatches == 23 &&
                provenance->evictedFuzzySearchSessions == 24 && provenance->evictedActivityRecords == 25 &&
                provenance->limits.maxSessions == 101 && provenance->limits.maxObservers == 102 &&
                provenance->limits.maxActiveOperations == 103 && provenance->limits.maxPendingRequests == 104 &&
                provenance->limits.maxRetainedThreads == 105 && provenance->limits.maxRetainedTurns == 106 &&
                provenance->limits.maxRetainedItems == 107 && provenance->limits.maxAccumulatedContentBytes == 108 &&
                provenance->limits.maxSnapshotBytes == 109 && provenance->limits.maxRetainedNotices == 110 &&
                provenance->limits.maxRetainedProcesses == 111 && provenance->limits.maxProcessOutputBytesPerProcess == 112 &&
                provenance->limits.maxAccumulatedProcessOutputBytes == 113 &&
                provenance->limits.maxRetainedFilesystemWatches == 114 &&
                provenance->limits.maxRetainedFuzzySearchSessions == 115 && provenance->limits.maxRetainedActivityRecords == 116 &&
                userMessageView && userMessageView->text == semanticUserText &&
                userMessageView->clientId == typed::ClientUserMessageId{"client-user-1"} && !userMessageView->textTruncated &&
                !userMessageView->contentTruncated && userMessageView->originalContentBytes == 128 &&
                userMessageView->retainedContentBytes == 128 && userMessageView->originalContentItems == 3 &&
                userMessageView->retainedContentItems == 3 && !commandAsUserMessage,
            "public additive semantic views preserve nested turn, realtime, item, request, domain, aggregate, and provenance facts");

        std::optional<client::UserMessageSemanticView> malformedUserMessageView;
        if (publicUserItem != nullptr) {
            client::ItemState malformed = *publicUserItem;
            malformed.data->erase("text");
            malformedUserMessageView = client::userMessageSemanticView(malformed);
        }
        result.expectTrue(!malformedUserMessageView,
                          "the typed user-message semantic view fails soft when its bounded scalar projection is missing");

        std::optional<client::UserMessageSemanticView> contradictoryUserMessageView;
        if (publicUserItem != nullptr) {
            client::ItemState malformed = *publicUserItem;
            (*malformed.data)["retainedContentBytes"] = 129;
            contradictoryUserMessageView = client::userMessageSemanticView(malformed);
        }
        result.expectTrue(!contradictoryUserMessageView,
                          "the typed user-message semantic view fails soft on contradictory retained-content metadata");

        client::State immutable = first.value_or(client::State{});
        publication.revision = 42;
        publication.snapshot = std::make_shared<const model::CanonicalSnapshot>(canonicalSnapshot(18, 4, "second title"));
        publication.visibleSequence = model::FrontendSequence{18};
        const auto second = buildCanonicalState(adopter, publication, std::numeric_limits<std::size_t>::max(), 64, error);
        const client::ThreadState* immutableThread = immutable.thread("adapter-thread");
        const client::ThreadState* secondThread = second ? second->thread("adapter-thread") : nullptr;
        result.expectTrue(second.has_value() && second->revision() == 42 && secondThread != nullptr &&
                              secondThread->title == std::optional<std::string>{"second title"} && immutable.revision() == 41 &&
                              immutableThread != nullptr && immutableThread->title == std::optional<std::string>{"first title"},
                          "a later direct canonical build leaves the prior public State immutable");

        client::detail::CanonicalStateBuildFailure capacityFailure = client::detail::CanonicalStateBuildFailure::StateDivergence;
        const auto rejected = buildCanonicalState(adopter, publication, 1, 64, error, &capacityFailure);
        result.expectTrue(!rejected.has_value() && !error.empty() &&
                              capacityFailure == client::detail::CanonicalStateBuildFailure::Capacity && immutable.revision() == 41 &&
                              immutableThread != nullptr && immutableThread->title == std::optional<std::string>{"first title"},
                          "public capacity preparation rejects before exposing a candidate and preserves the prior immutable State");
    }

    void testCanonicalLookupIdentityPreflight(tests::support::TestResult& result) {
        client::Client adopter(publicOptions());
        core::PublishedState publication;
        publication.revision = 1;
        publication.freshness = core::PublishedFreshness::Current;
        publication.representation = core::RepresentationMode::ExpandedV1;

        const auto rejects = [&adopter, &publication](model::CanonicalSnapshot snapshot, std::string_view expected) {
            publication.snapshot = std::make_shared<const model::CanonicalSnapshot>(std::move(snapshot));
            std::string error;
            client::detail::CanonicalStateBuildFailure failure = client::detail::CanonicalStateBuildFailure::Capacity;
            const auto built = buildCanonicalState(adopter, publication, std::numeric_limits<std::size_t>::max(), 64, error, &failure);
            return !built.has_value() && failure == client::detail::CanonicalStateBuildFailure::StateDivergence &&
                   error.find(expected) != std::string::npos;
        };

        bool allRejected = true;
        model::CanonicalSnapshot sessions = canonicalSnapshot(1, 1, "sessions");
        sessions.sessions.emplace_back(model::SessionIdentity{"7"});
        allRejected = rejects(std::move(sessions), "duplicate session identity") && allRejected;

        model::CanonicalSnapshot threads = canonicalSnapshot(1, 1, "threads");
        threads.threads.emplace_back(model::ThreadIdentity{"adapter-thread"});
        allRejected = rejects(std::move(threads), "duplicate thread identity") && allRejected;

        model::CanonicalSnapshot turns = canonicalSnapshot(1, 1, "turns");
        turns.turns.emplace_back(model::TurnIdentity{"duplicate-turn"}, model::ThreadIdentity{"adapter-thread"});
        turns.turns.emplace_back(model::TurnIdentity{"duplicate-turn"}, model::ThreadIdentity{"adapter-thread"});
        allRejected = rejects(std::move(turns), "duplicate turn identity") && allRejected;

        model::CanonicalSnapshot typedItems = canonicalSnapshot(1, 1, "typed-items");
        typedItems.items.push_back(model::AgentMessageItem{model::ItemData{model::ItemIdentity{"duplicate-item"}}});
        typedItems.items.push_back(model::UserMessageItem{model::ItemData{model::ItemIdentity{"duplicate-item"}}});
        allRejected = rejects(std::move(typedItems), "duplicate item identity") && allRejected;

        model::CanonicalSnapshot mixedItems = canonicalSnapshot(1, 1, "mixed-items");
        mixedItems.items.push_back(model::AgentMessageItem{model::ItemData{model::ItemIdentity{"mixed-item"}}});
        mixedItems.legacyItems.push_back({model::ItemData{model::ItemIdentity{"mixed-item"}}, "future_item", 1, "/items/1"});
        allRejected = rejects(std::move(mixedItems), "duplicate item identity") && allRejected;

        model::CanonicalSnapshot typedPending = canonicalSnapshot(1, 1, "typed-pending");
        typedPending.pendingRequests.push_back(model::UserInputRequest{model::PendingRequestData{model::PendingRequestIdentity{"72"}}});
        typedPending.pendingRequests.push_back(
            model::AuthenticationRequest{model::PendingRequestData{model::PendingRequestIdentity{"72"}}});
        allRejected = rejects(std::move(typedPending), "duplicate pending request identity") && allRejected;

        model::CanonicalSnapshot mixedPending = canonicalSnapshot(1, 1, "mixed-pending");
        mixedPending.pendingRequests.push_back(model::UserInputRequest{model::PendingRequestData{model::PendingRequestIdentity{"73"}}});
        mixedPending.legacyPendingRequests.push_back(
            {model::PendingRequestData{model::PendingRequestIdentity{"73"}}, 1, "/pendingRequests/1"});
        allRejected = rejects(std::move(mixedPending), "duplicate pending request identity") && allRejected;

        model::CanonicalSnapshot processes = canonicalSnapshot(1, 1, "processes");
        processes.processes.emplace_back(model::ProcessHandle{"duplicate-process"});
        processes.processes.emplace_back(model::ProcessHandle{"duplicate-process"});
        allRejected = rejects(std::move(processes), "duplicate process identity") && allRejected;

        model::CanonicalSnapshot watches = canonicalSnapshot(1, 1, "watches");
        model::FilesystemWatchRecord duplicateWatch;
        duplicateWatch.watchId = "duplicate-watch";
        watches.filesystemWatches.entries.push_back(duplicateWatch);
        watches.filesystemWatches.entries.push_back(std::move(duplicateWatch));
        allRejected = rejects(std::move(watches), "duplicate filesystem watch identity") && allRejected;

        model::CanonicalSnapshot searches = canonicalSnapshot(1, 1, "searches");
        model::FuzzySearchRecord duplicateSearch;
        duplicateSearch.sessionId = "duplicate-search";
        searches.fuzzySearches.entries.push_back(duplicateSearch);
        searches.fuzzySearches.entries.push_back(std::move(duplicateSearch));
        allRejected = rejects(std::move(searches), "duplicate fuzzy search identity") && allRejected;

        model::CanonicalSnapshot activities = canonicalSnapshot(1, 1, "activities");
        model::ActivityRecord duplicateActivity;
        duplicateActivity.key = "duplicate-activity";
        activities.activities.entries.push_back(duplicateActivity);
        activities.activities.entries.push_back(std::move(duplicateActivity));
        allRejected = rejects(std::move(activities), "duplicate activity identity") && allRejected;

        model::CanonicalSnapshot emptyActivity = canonicalSnapshot(1, 1, "empty-activity");
        emptyActivity.activities.entries.emplace_back();
        allRejected = rejects(std::move(emptyActivity), "empty activity identity") && allRejected;

        result.expectTrue(
            allRejected,
            "CanonicalStateBuilder rejects duplicate or empty public lookup identities, including typed/legacy overlap, before commit");
    }

    void testScopedItemIdentities(tests::support::TestResult& result) {
        client::Client adopter(publicOptions());
        core::PublishedState publication;
        publication.revision = 1;
        publication.freshness = core::PublishedFreshness::Current;
        publication.representation = core::RepresentationMode::ExpandedV1;

        model::CanonicalSnapshot snapshot = canonicalSnapshot(1, 1, "first thread");
        snapshot.threads.emplace_back(model::ThreadIdentity{"second-thread"});
        snapshot.turns.emplace_back(model::TurnIdentity{"first-turn"}, model::ThreadIdentity{"adapter-thread"});
        snapshot.turns.emplace_back(model::TurnIdentity{"second-turn"}, model::ThreadIdentity{"second-thread"});

        model::ItemData first{model::ItemIdentity{"item-1"},
                              model::ThreadIdentity{"adapter-thread"},
                              model::TurnIdentity{"first-turn"}};
        first.summary = "first scoped item";
        snapshot.items.push_back(model::UserMessageItem{std::move(first)});
        model::ItemData second{model::ItemIdentity{"item-1"},
                               model::ThreadIdentity{"second-thread"},
                               model::TurnIdentity{"second-turn"}};
        second.summary = "second scoped item";
        snapshot.items.push_back(model::AgentMessageItem{std::move(second)});
        publication.snapshot = std::make_shared<const model::CanonicalSnapshot>(std::move(snapshot));

        std::string error;
        const auto state = buildCanonicalState(adopter, publication, std::numeric_limits<std::size_t>::max(), 64, error);
        const client::ItemState* firstItem = state ? state->item(typed::ThreadId{"adapter-thread"},
                                                                 typed::TurnId{"first-turn"},
                                                                 typed::ItemId{"item-1"})
                                                   : nullptr;
        const client::ItemState* secondItem = state ? state->item(typed::ThreadId{"second-thread"},
                                                                  typed::TurnId{"second-turn"},
                                                                  typed::ItemId{"item-1"})
                                                    : nullptr;
        result.expectTrue(state.has_value() && error.empty() && state->items().size() == 2 && firstItem != nullptr &&
                              secondItem != nullptr && firstItem->summary == std::optional<std::string>{"first scoped item"} &&
                              secondItem->summary == std::optional<std::string>{"second scoped item"},
                          "CanonicalStateBuilder retains repeated provider item IDs under distinct thread and turn parents");
    }

    void testIndexedStateLookupsAndGroupedOrdering(tests::support::TestResult& result) {
        constexpr std::size_t ThreadCount = 128;
        constexpr std::size_t TurnsPerThread = 4;
        constexpr std::size_t ItemsPerTurn = 4;

        client::Client adopter(publicOptions());
        core::PublishedState publication;
        publication.revision = 1;
        publication.freshness = core::PublishedFreshness::Current;
        publication.representation = core::RepresentationMode::ExpandedV1;

        model::CanonicalSnapshot snapshot = canonicalSnapshot(1, 1, "indexed state");
        snapshot.threads.clear();
        snapshot.threads.reserve(ThreadCount);
        snapshot.turns.reserve(ThreadCount * TurnsPerThread);
        snapshot.items.reserve(ThreadCount * TurnsPerThread * ItemsPerTurn);
        snapshot.pendingRequests.reserve(ThreadCount);
        for (std::size_t threadIndex = 0; threadIndex < ThreadCount; ++threadIndex) {
            const std::string threadId = "indexed-thread-" + std::to_string(threadIndex);
            snapshot.threads.emplace_back(model::ThreadIdentity{threadId});
            for (std::size_t turnIndex = 0; turnIndex < TurnsPerThread; ++turnIndex) {
                const std::string turnId = threadId + "-turn-" + std::to_string(turnIndex);
                snapshot.turns.emplace_back(model::TurnIdentity{turnId}, model::ThreadIdentity{threadId});
                for (std::size_t itemIndex = 0; itemIndex < ItemsPerTurn; ++itemIndex) {
                    const std::string itemId = turnId + "-item-" + std::to_string(itemIndex);
                    model::ItemData item{
                        model::ItemIdentity{itemId}, model::ThreadIdentity{threadId}, model::TurnIdentity{turnId}};
                    item.summary = itemId;
                    item.sourceIndex = ItemsPerTurn - itemIndex - 1;
                    snapshot.items.push_back(model::AgentMessageItem{std::move(item)});
                }
            }
            const std::string requestId = "indexed-request-" + std::to_string(threadIndex);
            model::PendingRequestData request{model::PendingRequestIdentity{requestId}, model::ThreadIdentity{threadId}};
            request.summary = requestId;
            request.sourceIndex = ThreadCount - threadIndex - 1;
            snapshot.pendingRequests.push_back(model::CommandExecutionApprovalRequest{std::move(request)});
        }
        publication.snapshot = std::make_shared<const model::CanonicalSnapshot>(std::move(snapshot));

        std::string error;
        const auto state = buildCanonicalState(adopter, publication, std::numeric_limits<std::size_t>::max(), 64, error);
        bool exact = state.has_value() && error.empty() && state->threads().size() == ThreadCount &&
                     state->turns().size() == ThreadCount * TurnsPerThread &&
                     state->items().size() == ThreadCount * TurnsPerThread * ItemsPerTurn &&
                     state->pendingRequests().size() == ThreadCount;
        for (std::size_t threadIndex = 0; exact && threadIndex < ThreadCount; ++threadIndex) {
            const std::string threadId = "indexed-thread-" + std::to_string(threadIndex);
            const client::ThreadState* thread = state->thread(threadId);
            exact = thread != nullptr && thread->orderedTurns.size() == TurnsPerThread;
            for (std::size_t turnIndex = 0; exact && turnIndex < TurnsPerThread; ++turnIndex) {
                const std::string turnId = threadId + "-turn-" + std::to_string(turnIndex);
                exact = thread->orderedTurns[turnIndex] == typed::TurnId{turnId};
                const client::TurnState* turn = state->turn(turnId);
                exact = exact && turn != nullptr && turn->threadId == typed::ThreadId{threadId} &&
                        turn->orderedItems.size() == ItemsPerTurn;
                for (std::size_t orderedIndex = 0; exact && orderedIndex < ItemsPerTurn; ++orderedIndex) {
                    const std::size_t sourceItemIndex = ItemsPerTurn - orderedIndex - 1;
                    const std::string itemId = turnId + "-item-" + std::to_string(sourceItemIndex);
                    const client::ItemState* bare = state->item(itemId);
                    const client::ItemState* scoped =
                        state->item(typed::ThreadId{threadId}, typed::TurnId{turnId}, typed::ItemId{itemId});
                    exact = turn->orderedItems[orderedIndex] == typed::ItemId{itemId} && bare != nullptr && scoped == bare &&
                            scoped->summary == std::optional<std::string>{itemId};
                }
            }
            const std::string requestId = "indexed-request-" + std::to_string(threadIndex);
            const client::PendingRequestState* request = state->pendingRequest(client::PendingRequestId{requestId});
            exact = exact && request != nullptr && request->summary == std::optional<std::string>{requestId};
        }
        exact = exact && state->thread("missing-thread") == nullptr && state->turn("missing-turn") == nullptr &&
                state->item("missing-item") == nullptr &&
                state->item(typed::ThreadId{"missing-thread"}, typed::TurnId{"missing-turn"}, typed::ItemId{"missing-item"}) ==
                    nullptr &&
                state->pendingRequest(client::PendingRequestId{"missing-request"}) == nullptr;
        result.expectTrue(exact,
                          "large immutable State uses indexed identity lookup while preserving grouped thread/turn/item ordering");
    }

    void testScopedItemChanges(tests::support::TestResult& result) {
        PublicHarness harness;
        client::Client sdk(publicOptions(), harness.callbacks());
        harness.sdk = &sdk;
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        const bool synchronized = connection.receive(frontend::ServerMessage{welcome(7)}).accepted &&
                                  connection.receive(frontend::ServerMessage{expandedScopedItemsSnapshot(7)}).accepted &&
                                  connection.receive(frontend::ServerMessage{
                                      frontend::SyncComplete{frontend::SequenceNumber(7)}})
                                      .accepted;

        harness.changes.clear();
        const frontend::FrontendEvent upsert = scopedItemUpsertEvent(8);
        const bool upserted = connection
                                  .receive(frontend::ServerMessage{frontend::EventBatch{
                                      frontend::SequenceNumber(8), frontend::SequenceNumber(8), {upsert}}})
                                  .accepted;
        const auto* upsertChange = findChange<client::ItemUpsertedChange>(harness.changes);
        const client::ItemState* firstAfterUpsert = sdk.state().item(
            typed::ThreadId{"adapter-thread"}, typed::TurnId{"first-turn"}, typed::ItemId{"item-1"});
        const client::ItemState* secondAfterUpsert = sdk.state().item(
            typed::ThreadId{"second-thread"}, typed::TurnId{"second-turn"}, typed::ItemId{"item-1"});
        result.expectTrue(
            synchronized && upserted && upsertChange && upsertChange->itemId == typed::ItemId{"item-1"} &&
                upsertChange->threadId == std::optional<typed::ThreadId>{typed::ThreadId{"second-thread"}} &&
                upsertChange->turnId == std::optional<typed::TurnId>{typed::TurnId{"second-turn"}} && firstAfterUpsert &&
                firstAfterUpsert->agentText == std::optional<std::string>{"first scoped item"} && secondAfterUpsert &&
                secondAfterUpsert->agentText == std::optional<std::string>{"second upserted item"},
            "public item-upsert changes retain canonical thread/turn scope when bare item IDs repeat");

        harness.changes.clear();
        const frontend::FrontendEvent content = scopedItemContentEvent(9);
        const bool replaced = connection
                                  .receive(frontend::ServerMessage{frontend::EventBatch{
                                      frontend::SequenceNumber(9), frontend::SequenceNumber(9), {content}}})
                                  .accepted;
        const auto* contentChange = findChange<client::ItemContentReplacedChange>(harness.changes);
        const client::ItemState* firstAfterContent = sdk.state().item(
            typed::ThreadId{"adapter-thread"}, typed::TurnId{"first-turn"}, typed::ItemId{"item-1"});
        const client::ItemState* secondAfterContent = sdk.state().item(
            typed::ThreadId{"second-thread"}, typed::TurnId{"second-turn"}, typed::ItemId{"item-1"});
        result.expectTrue(
            replaced && contentChange && contentChange->itemId == typed::ItemId{"item-1"} &&
                contentChange->channel == client::ItemContentChannel::AgentText &&
                contentChange->threadId == std::optional<typed::ThreadId>{typed::ThreadId{"second-thread"}} &&
                contentChange->turnId == std::optional<typed::TurnId>{typed::TurnId{"second-turn"}} && firstAfterContent &&
                firstAfterContent->agentText == std::optional<std::string>{"first scoped item"} && secondAfterContent &&
                secondAfterContent->agentText == std::optional<std::string>{"second streamed item"},
            "public item-content changes retain canonical thread/turn scope when bare item IDs repeat");
    }

    void testHybridExpandedPublicationRetainsLegacyItems(tests::support::TestResult& result) {
        client::Client adopter(publicOptions());
        core::PublishedState publication;
        publication.revision = 1;
        publication.freshness = core::PublishedFreshness::Current;
        publication.representation = core::RepresentationMode::ExpandedV1;

        model::CanonicalSnapshot snapshot = canonicalSnapshot(1, 1, "hybrid-items");
        snapshot.turns.emplace_back(model::TurnIdentity{"hybrid-turn"}, model::ThreadIdentity{"adapter-thread"});
        model::ItemData item{
            model::ItemIdentity{"hybrid-future-item"}, model::ThreadIdentity{"adapter-thread"}, model::TurnIdentity{"hybrid-turn"}};
        snapshot.legacyItems.push_back({std::move(item), "future_item", 0, "/items/0"});
        publication.snapshot = std::make_shared<const model::CanonicalSnapshot>(std::move(snapshot));

        std::string error;
        const auto built = buildCanonicalState(adopter, publication, std::numeric_limits<std::size_t>::max(), 64, error);
        const client::ItemState* publicItem = built ? built->item("hybrid-future-item") : nullptr;
        const client::TurnState* publicTurn = built ? built->turn("hybrid-turn") : nullptr;
        result.expectTrue(
            built.has_value() && error.empty() && publicItem != nullptr && publicItem->kind.identity == "future_item" &&
                !publicItem->kind.known.has_value() && publicTurn != nullptr && publicTurn->orderedItems.size() == 1 &&
                publicTurn->orderedItems.front().value == "hybrid-future-item",
            "an ExpandedV1 publication with legacy-compatible items preserves the independently negotiated item representation");
    }

    void testPublicClientCoreAdapter(tests::support::TestResult& result) {
        PublicHarness harness;
        client::Client sdk(publicOptions(), harness.callbacks());
        harness.sdk = &sdk;
        client::Connection connection = sdk.openConnection(harness.transport());
        const std::uint64_t firstGeneration = connection.generation();
        connection.transportConnected();
        connection.transportConnected();

        const auto hello = harness.outbound.empty() ? frontend::Codec::decodeClient(std::string_view{})
                                                    : frontend::Codec::decodeClient(std::string_view(harness.outbound.front().compactJson));
        result.expectTrue(firstGeneration == 1 && harness.outbound.size() == 1 && hello &&
                              std::holds_alternative<frontend::Hello>(hello.value()) &&
                              sdk.connectionState() == client::ConnectionState::Authenticating,
                          "public Connection delegates one physical generation and one transport-connected transition to ClientCore");

        const bool welcomeAccepted = connection.receive(frontend::ServerMessage{welcome(7)}).accepted;
        const frontend::Snapshot initialSnapshot = expandedUserMessageSnapshot(7, 3, "runtime title");
        const auto fixtureDecoded = model::decodeProjectedSnapshot(initialSnapshot, publicOptions().requestedCapabilities);
        const std::string fixtureError = fixtureDecoded ? std::string{} : fixtureDecoded.error().message;
        const bool snapshotAccepted = connection.receive(frontend::ServerMessage{initialSnapshot}).accepted;
        harness.recording = true;
        const bool synchronized = connection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(7)}}).accepted;
        harness.recording = false;
        const client::State ready = sdk.state();
        const std::optional<client::SessionInfo> readySession = sdk.session();
        const client::ItemState* readyUserItem = ready.item("runtime-user-item");
        const auto readyUserMessage = readyUserItem ? client::userMessageSemanticView(*readyUserItem) : std::nullopt;
        const std::string expectedRetainedText = std::string(16'383, 'm') + "\xE2\x82\xAC";
        const std::vector<std::string> expectedSynchronizationOrder{"ready", "state", "cursor", "synchronized", "protocol"};
        result.expectTrue(
            welcomeAccepted && snapshotAccepted && synchronized && sdk.isReady() && !harness.revisionMismatch &&
                harness.readySawCommittedState && harness.callbackOrder == expectedSynchronizationOrder &&
                !harness.updateRevisions.empty() && !harness.synchronizedRevisions.empty() &&
                harness.updateRevisions.back() == ready.revision() && harness.synchronizedRevisions.back() == ready.revision() &&
                readySession &&
                readySession->serverMaximumInboundMessageBytes == std::optional<std::uint64_t>{768U * 1024U} &&
                ready.visibleSequence() == frontend::SequenceNumber(7) && ready.thread("adapter-thread") != nullptr &&
                ready.thread("adapter-thread")->title == std::optional<std::string>{"runtime title"} && readyUserMessage &&
                readyUserMessage->text == expectedRetainedText && readyUserMessage->textTruncated &&
                !readyUserMessage->contentTruncated && readyUserMessage->originalContentBytes == readyUserMessage->retainedContentBytes &&
                readyUserMessage->originalContentItems == 1 && readyUserMessage->retainedContentItems == 1,
            "ClientCore commits the direct public State before state/cursor/synchronized/protocol callbacks in frozen order: " +
                traceText(harness.callbackOrder) + " accepted=" + std::to_string(welcomeAccepted) + "/" + std::to_string(snapshotAccepted) +
                "/" + std::to_string(synchronized) + " state=" + std::to_string(static_cast<int>(sdk.connectionState())) + " fixture=" +
                fixtureError + " fixture-state=" + initialSnapshot.state.dump() + " diagnostics=" + traceText(harness.diagnostics));

        harness.callbackOrder.clear();
        harness.recording = true;
        const frontend::FrontendEvent live = providerEvent(8, 9);
        const bool liveAccepted =
            connection
                .receive(frontend::ServerMessage{frontend::EventBatch{frontend::SequenceNumber(8), frontend::SequenceNumber(8), {live}}})
                .accepted;
        harness.recording = false;
        const client::State afterLive = sdk.state();
        const std::vector<std::string> expectedLiveOrder{"state", "cursor", "protocol"};
        result.expectTrue(
            liveAccepted && harness.callbackOrder == expectedLiveOrder && ready.revision() != std::numeric_limits<std::uint64_t>::max() &&
                afterLive.revision() == ready.revision() + 1 && afterLive.provider().value.has_value() &&
                afterLive.provider().value->generation == 9 && ready.provider().value.has_value() &&
                ready.provider().value->generation == 3 && ready.revision() != afterLive.revision() && !harness.revisionMismatch,
            "live ClientCore publication advances one public revision while the prior State remains immutable: " +
                traceText(harness.callbackOrder));

        const client::State beforeDisconnect = afterLive;
        connection.transportDisconnected(client::TransportError{"cycle one ended", true});
        const client::State retainedStale = sdk.state();
        result.expectTrue(
            beforeDisconnect.revision() != std::numeric_limits<std::uint64_t>::max() &&
                retainedStale.revision() == beforeDisconnect.revision() + 1 && retainedStale.freshness() == client::StateFreshness::Stale &&
                beforeDisconnect.freshness() == client::StateFreshness::Current && beforeDisconnect.provider().value.has_value() &&
                beforeDisconnect.provider().value->generation == 9 && !harness.revisionMismatch,
            "public Client exposes the ClientCore retained-stale N+1 revision without mutating its prior State");
        client::Connection replacement = sdk.openConnection(harness.transport());
        const std::size_t observedBeforeStale = harness.protocolMessages;
        const auto stale = connection.receive(frontend::ServerMessage{frontend::ProtocolErrorMessage{
            frontend::ErrorCode::RateLimited, "retired generation", {}, false, std::nullopt, std::nullopt, frontend::Json::object()}});
        result.expectTrue(!stale.accepted && replacement.generation() == firstGeneration + 1 &&
                              sdk.connectionState() == client::ConnectionState::Connecting &&
                              harness.protocolMessages == observedBeforeStale,
                          "a retired public Connection cannot continue into the next ClientCore physical generation");

        client::ClientOptions oneByteOptions = publicOptions();
        oneByteOptions.maximumOutboundMessageBytes = 1;
        PublicHarness oneByteHarness;
        client::Client oneByteClient(std::move(oneByteOptions), oneByteHarness.callbacks());
        oneByteHarness.sdk = &oneByteClient;
        client::Connection oneByteConnection = oneByteClient.openConnection(oneByteHarness.transport());
        oneByteConnection.transportConnected();
        result.expectTrue(oneByteClient.connectionState() == client::ConnectionState::Disconnected &&
                              oneByteHarness.outbound.empty() && oneByteHarness.closes == 1,
                          "the public configured fallback bounds the exact final encoded Hello before transport delivery");
    }

    void testClosingProtocolErrorReceiveResult(tests::support::TestResult& result) {
        struct Case {
            frontend::ErrorCode code;
            bool retryable;
        };
        constexpr std::array cases{
            Case{frontend::ErrorCode::InternalError, true},
            Case{frontend::ErrorCode::InvalidCommand, false},
        };

        for (const Case& testCase : cases) {
            PublicHarness harness;
            client::Client sdk(publicOptions(), harness.callbacks());
            harness.sdk = &sdk;
            client::Connection connection = sdk.openConnection(harness.transport());
            connection.transportConnected();

            frontend::ProtocolErrorMessage closing;
            closing.code = testCase.code;
            closing.message = "public classified closing protocol error";
            closing.closeConnection = true;
            const auto encoded = frontend::Codec::encodeServer(frontend::ServerMessage{std::move(closing)});
            const std::string compact = encoded ? encoded.value().dump() : std::string{};
            const client::ReceiveResult received = connection.receive(std::string_view(compact));
            const std::optional<client::Error> terminal =
                harness.connectionErrors.empty() ? std::nullopt : std::optional<client::Error>{harness.connectionErrors.back()};

            result.expectTrue(encoded && received.accepted && !received.error.has_value() && !connection.isOpen() &&
                                  sdk.connectionState() == client::ConnectionState::Disconnected && harness.closes == 1 && terminal &&
                                  terminal->origin == client::ErrorOrigin::Protocol && terminal->protocolCode == testCase.code &&
                                  terminal->retryable == testCase.retryable,
                              "public receive accepts a closing protocol frame without replacing its retry classification: " +
                                  std::string(frontend::toString(testCase.code)));
        }
    }

    void testTransactionalPreparationFailure(tests::support::TestResult& result) {
        core::ClientOptions options;
        options.credentialProvider = [] {
            return core::AuthenticationContext{frontend::NoCredential{}, std::string{"prepare-failure"}};
        };
        std::size_t prepared = 0;
        std::size_t committed = 0;
        std::size_t published = 0;
        std::size_t readyTransitions = 0;
        bool currentCommitted = false;
        std::vector<std::string> diagnostics;
        core::ClientCallbacks callbacks;
        callbacks.prepareStatePublication = [&prepared](const core::PublishedState& candidate) -> std::optional<core::ClientError> {
            ++prepared;
            if (candidate.freshness == core::PublishedFreshness::Current) {
                return core::ClientError{core::ErrorOrigin::Protocol,
                                         core::ClientErrorCode::StateCapacityExceeded,
                                         frontend::ErrorCode::CapacityExceeded,
                                         "injected public State capacity rejection",
                                         std::nullopt,
                                         false};
            }
            return std::nullopt;
        };
        callbacks.commitStatePublication = [&committed, &currentCommitted](const core::PublishedState& candidate) {
            ++committed;
            currentCommitted = currentCommitted || candidate.freshness == core::PublishedFreshness::Current;
        };
        callbacks.onStatePublished = [&published](const std::shared_ptr<const core::PublishedState>&) {
            ++published;
        };
        callbacks.onDiagnostic = [&diagnostics](const core::Diagnostic& diagnostic) {
            diagnostics.push_back(diagnostic.message);
        };
        callbacks.onConnectionStateChanged = [&diagnostics, &readyTransitions](const core::StateChange& change) {
            if (change.current == core::ConnectionState::Ready) {
                ++readyTransitions;
            }
            if (change.error.has_value()) {
                diagnostics.push_back(change.error->message);
            }
        };
        core::ClientCore sdk(std::move(options), std::move(callbacks));
        std::vector<core::OutboundMessage> outbound;
        const auto generation = sdk.attach({[&outbound](core::OutboundMessage message) {
                                                outbound.push_back(std::move(message));
                                                return core::SendResult{};
                                            },
                                            [](std::string_view) {
                                            }});
        sdk.transportConnected(*generation);
        const bool welcomeAccepted = sdk.receive(*generation, frontend::ServerMessage{welcome(7)});
        const bool snapshotAccepted = sdk.receive(*generation, frontend::ServerMessage{expandedSnapshot(7, 3, "rejected title")});
        const bool accepted = sdk.receive(*generation, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(7)}});
        result.expectTrue(!accepted && prepared == 3 && committed == 2 && published == 2 && readyTransitions == 0 && !currentCommitted &&
                              sdk.state()->revision == 2 && sdk.state()->freshness == core::PublishedFreshness::Stale &&
                              sdk.connectionState() != core::ConnectionState::Ready,
                          "final SyncComplete preparation rejects before any transient Ready while preserving the last committed revision; "
                          "prepared=" +
                              std::to_string(prepared) + " committed=" + std::to_string(committed) +
                              " published=" + std::to_string(published) + " revision=" + std::to_string(sdk.state()->revision) +
                              " ready=" + std::to_string(readyTransitions) + " accepted=" + std::to_string(welcomeAccepted) + "/" +
                              std::to_string(snapshotAccepted) + "/" + std::to_string(accepted) + " diagnostics=" + traceText(diagnostics));
    }

    void testTransactionalStaleFallback(tests::support::TestResult& result) {
        core::ClientOptions options;
        options.credentialProvider = [] {
            return core::AuthenticationContext{frontend::NoCredential{}, std::string{"stale-fallback"}};
        };
        std::size_t staleRejections = 0;
        std::size_t emptyStaleCommits = 0;
        core::ClientCallbacks callbacks;
        callbacks.prepareStatePublication = [&staleRejections](const core::PublishedState& candidate) -> std::optional<core::ClientError> {
            if (candidate.freshness == core::PublishedFreshness::Stale && candidate.snapshot) {
                ++staleRejections;
                return core::ClientError{core::ErrorOrigin::Protocol,
                                         core::ClientErrorCode::StateCapacityExceeded,
                                         frontend::ErrorCode::CapacityExceeded,
                                         "injected retained stale capacity rejection",
                                         std::nullopt,
                                         false};
            }
            return std::nullopt;
        };
        callbacks.commitStatePublication = [&emptyStaleCommits](const core::PublishedState& committed) {
            if (committed.freshness == core::PublishedFreshness::Stale && !committed.snapshot && !committed.session) {
                ++emptyStaleCommits;
            }
        };
        core::ClientCore sdk(std::move(options), std::move(callbacks));
        const auto generation = sdk.attach({[](core::OutboundMessage) {
                                                return core::SendResult{};
                                            },
                                            [](std::string_view) {
                                            }});
        sdk.transportConnected(*generation);
        const bool welcomeAccepted = sdk.receive(*generation, frontend::ServerMessage{welcome(7)});
        const bool snapshotAccepted = sdk.receive(*generation, frontend::ServerMessage{expandedSnapshot(7, 3, "stale title")});
        const bool synchronized = sdk.receive(*generation, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(7)}});
        const std::shared_ptr<const core::PublishedState> prior = sdk.state();
        const std::uint64_t priorRevision = prior->revision;
        const std::string priorTitle = prior->snapshot && !prior->snapshot->threads.empty()
                                           ? prior->snapshot->threads.front().title.value_or(std::string{})
                                           : std::string{};
        sdk.transportDisconnected(*generation, core::TransportError{"capacity boundary disconnect", true});
        const std::shared_ptr<const core::PublishedState> stale = sdk.state();
        result.expectTrue(welcomeAccepted && snapshotAccepted && synchronized && staleRejections == 1 && emptyStaleCommits == 1 &&
                              priorRevision != std::numeric_limits<std::uint64_t>::max() && stale && stale->revision == priorRevision + 1 &&
                              stale->freshness == core::PublishedFreshness::Stale && !stale->snapshot && !stale->session &&
                              sdk.connectionState() == core::ConnectionState::Disconnected,
                          "a retained stale preparation failure atomically publishes the bounded empty stale fallback at revision N+1: "
                          "prior=" +
                              std::to_string(priorRevision) +
                              " fallback=" + std::to_string(stale ? stale->revision : std::numeric_limits<std::uint64_t>::max()));
        result.expectTrue(stale != prior && prior->revision == priorRevision && prior->freshness == core::PublishedFreshness::Current &&
                              prior->snapshot && !prior->snapshot->threads.empty() &&
                              prior->snapshot->threads.front().title.value_or(std::string{}) == priorTitle,
                          "bounded-empty stale fallback leaves the prior published State immutable");
    }

} // namespace

int main() {
    tests::support::TestResult result;
    testDirectCanonicalStateBuilder(result);
    testCanonicalLookupIdentityPreflight(result);
    testScopedItemIdentities(result);
    testIndexedStateLookupsAndGroupedOrdering(result);
    testScopedItemChanges(result);
    testHybridExpandedPublicationRetainsLegacyItems(result);
    testPublicClientCoreAdapter(result);
    testClosingProtocolErrorReceiveResult(result);
    testTransactionalPreparationFailure(result);
    testTransactionalStaleFallback(result);
    return result.processResult();
}
