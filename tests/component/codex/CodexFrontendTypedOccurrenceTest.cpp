/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/internal/model/Occurrence.h"
#include "support/TestResult.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = frontend::generated;
    namespace model = frontend::internal::model;

    model::OccurrenceDecodeContext context(std::uint32_t index = 0, std::uint32_t count = 1) {
        return {*model::OccurrenceGroupIdentity::parse("group-1"),
                index,
                count,
                *model::SourceStamp::parse("server_notification:ServerNotification:method:future/unknownNotification")};
    }

    void testLegacyPendingRoundTrips(tests::support::TestResult& result) {
        const frontend::FrontendEvent pending{frontend::SequenceNumber{4},
                                              "request.pending",
                                              {{"request", {{"id", "7"}, {"type", "command_approval"}, {"details", {{"safe", true}}}}}}};
        const frontend::FrontendEvent resolved{
            frontend::SequenceNumber{5}, "request.resolved", {{"pendingRequestId", "7"}, {"reason", "completed"}}};
        const auto decodedPending = model::decodeLegacyOccurrence(pending, context());
        const auto decodedResolved = model::decodeLegacyOccurrence(resolved, context());
        const auto encodedPending = decodedPending ? model::encodeLegacyOccurrence(decodedPending.value())
                                                   : model::OccurrenceResult<frontend::FrontendEvent>{decodedPending.error()};
        const auto encodedResolved = decodedResolved ? model::encodeLegacyOccurrence(decodedResolved.value())
                                                     : model::OccurrenceResult<frontend::FrontendEvent>{decodedResolved.error()};
        result.expectTrue(
            encodedPending && encodedResolved && encodedPending.value().type == "request.pending" &&
                encodedPending.value().data.at("request").at("id") == "7" && encodedResolved.value().type == "request.resolved" &&
                encodedResolved.value().data.at("pendingRequestId") == "7" && encodedResolved.value().data.at("reason") == "completed",
            "legacy pending and resolved occurrences preserve their distinct canonical compatibility descriptors");
    }

    void testContainedExtension(tests::support::TestResult& result) {
        const frontend::FrontendEvent extension{
            frontend::SequenceNumber{6}, "codex.extension", {{"method", "future/unknownNotification"}, {"safe", "retained"}}};
        const auto decoded = model::decodeLegacyOccurrence(extension, context());
        const auto expanded = decoded ? model::encodeExpandedOccurrence(decoded.value())
                                      : model::OccurrenceResult<std::vector<frontend::ExpandedFrontendEvent>>{decoded.error()};
        const auto encoded =
            decoded ? model::encodeLegacyOccurrence(decoded.value()) : model::OccurrenceResult<frontend::FrontendEvent>{decoded.error()};
        const auto merged = decoded ? model::mergeOccurrenceGroup(std::span<const model::CanonicalOccurrence>{&decoded.value(), 1})
                                    : model::OccurrenceResult<model::CanonicalOccurrence>{decoded.error()};
        model::CanonicalSnapshot snapshot;
        const auto reduced =
            decoded ? model::reduceOccurrence(snapshot, decoded.value())
                    : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "decode failed"}};
        result.expectTrue(decoded && decoded.value().expandedPayloads().empty() && expanded && expanded.value().empty() && encoded &&
                              merged && merged.value() == decoded.value() && encoded.value().data.value("safe", "") == "retained" &&
                              reduced && reduced.value().sequence == model::FrontendSequence{6} &&
                              reduced.value().stateExtensions.json().at("codexExtensions").size() == 1,
                          "unknown notifications remain one bounded legacy fallback and reduce as an observable semantic no-op");
    }

    void testLegacyExtensionWireShape(tests::support::TestResult& result) {
        const frontend::Json data{{"method", "future/truncated"},
                                  {"params", frontend::Json::object()},
                                  {"decodingError", "bounded"},
                                  {"sensitiveFieldsRedacted", true},
                                  {"truncation",
                                   {{"method", {{"originalBytes", std::uint64_t{30}}, {"retainedBytes", std::uint64_t{16}}}},
                                    {"params", {{"originalBytes", std::uint64_t{90}}}},
                                    {"decodingError", {{"originalBytes", std::uint64_t{20}}, {"retainedBytes", std::uint64_t{7}}}}}}};
        const frontend::FrontendEvent event{frontend::SequenceNumber{7}, "codex.extension", data};
        const auto decoded = model::decodeLegacyOccurrence(event, context());
        const auto encoded =
            decoded ? model::encodeLegacyOccurrence(decoded.value()) : model::OccurrenceResult<frontend::FrontendEvent>{decoded.error()};
        const model::LegacySafeExtension* extension = decoded && decoded.value().legacyCompatibility().safeExtension
                                                          ? &*decoded.value().legacyCompatibility().safeExtension
                                                          : nullptr;
        result.expectTrue(encoded && encoded.value().data == data && extension != nullptr && extension->paramsKnown &&
                              extension->wireTruncation.method.has_value() && extension->wireTruncation.params.has_value() &&
                              extension->wireTruncation.decodingError.has_value() && extension->truncation.truncated &&
                              extension->truncation.droppedBytes == 115,
                          "legacy extension occurrence retains empty params and typed per-field truncation metadata exactly");
    }

    void testGeneratedMultiFamilyGroup(tests::support::TestResult& result) {
        const auto metadata = std::find_if(generated::AllNotificationProjections.begin(),
                                           generated::AllNotificationProjections.end(),
                                           [](const generated::ProjectionMetadata& value) {
                                               return value.expandedMappings.size() == 2 &&
                                                      value.expandedMappings[0] == "configuration.updated" &&
                                                      value.expandedMappings[1] == "notice.added";
                                           });
        model::ConfigurationState configuration;
        configuration.state = model::DomainState::present();
        model::NoticeRecord notice;
        notice.occurrence = 8;
        notice.category = "configuration";
        notice.summary = "warning";
        model::LegacyCompatibilityPayload legacy;
        legacy.kind = model::LegacyCompatibilityKind::CodexExtension;
        model::LegacySafeExtension extension;
        extension.method = "configWarning";
        legacy.safeExtension = std::move(extension);
        const std::string source = metadata == generated::AllNotificationProjections.end() ? std::string{"missing-generated-authority"}
                                                                                           : std::string{metadata->registryKey};
        model::OccurrenceIdentity identity{
            model::FrontendSequence{8}, *model::OccurrenceGroupIdentity::parse("group-8"), 0, 2, *model::SourceStamp::parse(source)};
        const auto occurrence = model::makeOccurrenceGroup(
            std::move(identity),
            std::move(legacy),
            {model::ConfigurationUpdatedOccurrence{std::move(configuration)}, model::NoticeAddedOccurrence{std::move(notice)}});
        const auto expanded = occurrence ? model::encodeExpandedOccurrence(occurrence.value())
                                         : model::OccurrenceResult<std::vector<frontend::ExpandedFrontendEvent>>{occurrence.error()};
        const auto compatibility = occurrence ? model::encodeLegacyOccurrence(occurrence.value())
                                              : model::OccurrenceResult<frontend::FrontendEvent>{occurrence.error()};
        result.expectTrue(metadata != generated::AllNotificationProjections.end() && occurrence && expanded &&
                              expanded.value().size() == 2 && expanded.value().front().sequence == expanded.value().back().sequence &&
                              compatibility && compatibility.value().type == "codex.extension" &&
                              compatibility.value().data.value("method", "") == "configWarning",
                          "generated multi-family mappings share one sequence and one preserved legacy descriptor");
    }

    model::CanonicalSnapshot snapshotWithItem() {
        model::CanonicalSnapshot snapshot;
        model::ItemData item{model::ItemIdentity{"item-optional-content"},
                             model::ThreadIdentity{"thread-optional-content"},
                             model::TurnIdentity{"turn-optional-content"}};
        item.agentText = "before";
        item.contentTruncated = true;
        item.droppedContentBytes = 17;
        snapshot.items.emplace_back(model::AgentMessageItem{std::move(item)});
        return snapshot;
    }

    void testItemContentPresence(tests::support::TestResult& result) {
        const frontend::ExpandedFrontendEvent omitted{frontend::SequenceNumber{9},
                                                      frontend::ExpandedEventType::ItemContentUpdated,
                                                      {{"threadId", "thread-optional-content"},
                                                       {"turnId", "turn-optional-content"},
                                                       {"itemId", "item-optional-content"},
                                                       {"channel", "agentText"},
                                                       {"content", "omitted metadata"}}};
        const frontend::ExpandedFrontendEvent explicitReset{frontend::SequenceNumber{10},
                                                            frontend::ExpandedEventType::ItemContentUpdated,
                                                            {{"threadId", "thread-optional-content"},
                                                             {"turnId", "turn-optional-content"},
                                                             {"itemId", "item-optional-content"},
                                                             {"channel", "agentText"},
                                                             {"content", "explicit reset"},
                                                             {"contentTruncated", false},
                                                             {"droppedContentBytes", std::uint64_t{0}}}};
        const auto omittedOccurrence = model::decodeExpandedOccurrence(omitted, context());
        const auto resetOccurrence = model::decodeExpandedOccurrence(explicitReset, context());
        const auto omittedWire = omittedOccurrence
                                     ? model::encodeExpandedOccurrence(omittedOccurrence.value())
                                     : model::OccurrenceResult<std::vector<frontend::ExpandedFrontendEvent>>{omittedOccurrence.error()};
        const auto preserved =
            omittedOccurrence
                ? model::reduceOccurrence(snapshotWithItem(), omittedOccurrence.value())
                : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "decode failed"}};
        const auto reset =
            resetOccurrence
                ? model::reduceOccurrence(snapshotWithItem(), resetOccurrence.value())
                : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "decode failed"}};
        const model::ItemData* preservedItem =
            preserved && !preserved.value().items.empty() ? &model::itemData(preserved.value().items.front()) : nullptr;
        const model::ItemData* resetItem = reset && !reset.value().items.empty() ? &model::itemData(reset.value().items.front()) : nullptr;
        result.expectTrue(omittedOccurrence && resetOccurrence, "item-content occurrences decode omitted and explicit truncation metadata");
        result.expectTrue(omittedWire && omittedWire.value().size() == 1 &&
                              !omittedWire.value().front().data.contains("contentTruncated") &&
                              !omittedWire.value().front().data.contains("droppedContentBytes"),
                          "item-content occurrence encoding preserves omitted truncation metadata");
        result.expectTrue(preservedItem != nullptr && preservedItem->contentTruncated &&
                              preservedItem->droppedContentBytes == std::optional<std::uint64_t>{17},
                          "item-content reduction preserves previously known truncation metadata when omitted");
        result.expectTrue(resetItem != nullptr && !resetItem->contentTruncated &&
                              resetItem->droppedContentBytes == std::optional<std::uint64_t>{0},
                          "item-content reduction applies an explicit false/zero truncation reset");
    }

    void testDiagnosticOccurrenceShape(tests::support::TestResult& result) {
        const frontend::ExpandedFrontendEvent expanded{
            frontend::SequenceNumber{11}, frontend::ExpandedEventType::DiagnosticsUpdated, {{"diagnostic", frontend::Json::object()}}};
        const frontend::FrontendEvent legacy{
            frontend::SequenceNumber{12}, "diagnostics.updated", {{"received", std::uint64_t{0}}, {"recent", frontend::Json::array()}}};
        const auto expandedOccurrence = model::decodeExpandedOccurrence(expanded, context());
        const auto legacyOccurrence = model::decodeLegacyOccurrence(legacy, context());
        const auto expandedState =
            expandedOccurrence
                ? model::reduceOccurrence(model::CanonicalSnapshot{}, expandedOccurrence.value())
                : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "decode failed"}};
        const auto legacyState =
            legacyOccurrence
                ? model::reduceOccurrence(model::CanonicalSnapshot{}, legacyOccurrence.value())
                : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "decode failed"}};
        result.expectTrue(expandedState && legacyState && expandedState.value().diagnostics.entries.size() == 1 &&
                              legacyState.value().diagnostics.entries.empty() &&
                              legacyState.value().diagnostics.state.information == model::InformationState::Present,
                          "a single expanded diagnostic remains distinct from an empty legacy aggregate update");
    }

    void testLegacyNestedOccurrenceRoundTrips(tests::support::TestResult& result) {
        const frontend::Json item{{"id", "item-legacy"},
                                  {"type", "agent_message"},
                                  {"status", "streaming"},
                                  {"agentText", "hello"},
                                  {"reasoningText", ""},
                                  {"reasoningSummary", ""},
                                  {"commandOutput", ""},
                                  {"droppedContentBytes", std::uint64_t{0}},
                                  {"contentTruncated", false},
                                  {"data", frontend::Json::object()},
                                  {"extensions", {{"safeItemExtension", true}}}};
        const frontend::Json turn{{"id", "turn-legacy"},
                                  {"threadId", "thread-legacy"},
                                  {"status", "inProgress"},
                                  {"active", true},
                                  {"terminal", false},
                                  {"items", frontend::Json::array({item})},
                                  {"extensions", {{"safeTurnExtension", true}}}};
        const frontend::Json thread{{"id", "thread-legacy"},
                                    {"fullyLoaded", true},
                                    {"cwd", "/workspace"},
                                    {"turns", frontend::Json::array({turn})},
                                    {"extensions", {{"safeThreadExtension", true}}}};
        const frontend::FrontendEvent event{frontend::SequenceNumber{13}, "thread.updated", {{"thread", thread}}};
        const auto decoded = model::decodeLegacyOccurrence(event, context());
        const auto encoded =
            decoded ? model::encodeLegacyOccurrence(decoded.value()) : model::OccurrenceResult<frontend::FrontendEvent>{decoded.error()};

        model::CanonicalSnapshot before;
        before.turns.emplace_back(model::TurnIdentity{"old-turn"}, model::ThreadIdentity{"thread-legacy"});
        model::ItemData oldItem{model::ItemIdentity{"old-item"}, model::ThreadIdentity{"thread-legacy"}, model::TurnIdentity{"old-turn"}};
        before.items.emplace_back(model::AgentMessageItem{std::move(oldItem)});
        const auto reduced =
            decoded ? model::reduceOccurrence(before, decoded.value())
                    : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "decode failed"}};
        const model::ItemData* reducedItem =
            reduced && reduced.value().items.size() == 1 ? &model::itemData(reduced.value().items.front()) : nullptr;
        result.expectTrue(decoded && encoded && encoded.value().data == event.data && reduced && reduced.value().turns.size() == 1 &&
                              reduced.value().turns.front().id.value() == "turn-legacy" && reducedItem != nullptr &&
                              reducedItem->id.value() == "item-legacy" &&
                              model::threadItemKind(reduced.value().items.front()) == frontend::ThreadItemKind::AgentMessage,
                          "legacy thread occurrences preserve nested turns, snake-case items, extensions, and replacement semantics");
    }

    void testLegacyPendingPresentationAndDiagnostics(tests::support::TestResult& result) {
        const frontend::Json question{{"id", "question-1"},
                                      {"header", "Choice"},
                                      {"prompt", "Choose"},
                                      {"allowsFreeText", true},
                                      {"secret", false},
                                      {"options", frontend::Json::array({{{"label", "A"}, {"description", "first"}}})}};
        const frontend::FrontendEvent pending{
            frontend::SequenceNumber{14},
            "request.pending",
            {{"request",
              {{"id", "14"},
               {"type", "user_input"},
               {"threadId", "thread-legacy"},
               {"turnId", "turn-legacy"},
               {"itemId", "item-legacy"},
               {"details", {{"questions", frontend::Json::array({question})}, {"autoResolutionMs", std::uint64_t{1000}}}}}}}};
        const frontend::FrontendEvent diagnostics{frontend::SequenceNumber{15},
                                                  "diagnostics.updated",
                                                  {{"received", std::uint64_t{2}}, {"recent", frontend::Json::array({"first", "second"})}}};
        const frontend::FrontendEvent unknownPending{
            frontend::SequenceNumber{16},
            "request.pending",
            {{"request", {{"id", "16"}, {"type", "unknown"}, {"details", frontend::Json::object()}}}}};
        const auto decodedPending = model::decodeLegacyOccurrence(pending, context());
        const auto encodedPending = decodedPending ? model::encodeLegacyOccurrence(decodedPending.value())
                                                   : model::OccurrenceResult<frontend::FrontendEvent>{decodedPending.error()};
        const auto decodedDiagnostics = model::decodeLegacyOccurrence(diagnostics, context());
        const auto encodedDiagnostics = decodedDiagnostics ? model::encodeLegacyOccurrence(decodedDiagnostics.value())
                                                           : model::OccurrenceResult<frontend::FrontendEvent>{decodedDiagnostics.error()};
        const auto decodedUnknown = model::decodeLegacyOccurrence(unknownPending, context());
        const auto encodedUnknown = decodedUnknown ? model::encodeLegacyOccurrence(decodedUnknown.value())
                                                   : model::OccurrenceResult<frontend::FrontendEvent>{decodedUnknown.error()};
        model::CanonicalSnapshot before;
        model::DiagnosticRecord old;
        old.message = "old";
        before.diagnostics.entries.push_back(std::move(old));
        const auto reducedDiagnostics =
            decodedDiagnostics
                ? model::reduceOccurrence(before, decodedDiagnostics.value())
                : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "decode failed"}};
        const model::PendingRequestData* request = decodedPending && !decodedPending.value().expandedPayloads().empty()
                                                       ? &model::pendingRequestData(std::get<model::PendingRequestsUpdatedOccurrence>(
                                                                                        decodedPending.value().expandedPayloads().front())
                                                                                        .pendingRequests.front())
                                                       : nullptr;
        result.expectTrue(encodedPending && encodedPending.value().data == pending.data && request != nullptr &&
                              request->questions.size() == 1 && request->autoResolutionMs == std::optional<std::uint64_t>{1000} &&
                              encodedDiagnostics && encodedDiagnostics.value().data == diagnostics.data && reducedDiagnostics &&
                              reducedDiagnostics.value().diagnostics.entries.size() == 2 &&
                              reducedDiagnostics.value().diagnostics.entries.front().message == std::optional<std::string>{"first"} &&
                              decodedUnknown && decodedUnknown.value().expandedPayloads().empty() &&
                              decodedUnknown.value().legacyCompatibility().legacyPendingRequest.has_value() && encodedUnknown &&
                              encodedUnknown.value().data == unknownPending.data,
                          "legacy pending presentation fields and diagnostic aggregates retain their frozen wire and reducer semantics");
    }

    void testOccurrenceIdentityValidation(tests::support::TestResult& result) {
        const frontend::ExpandedFrontendEvent wrongParent{frontend::SequenceNumber{17},
                                                          frontend::ExpandedEventType::ItemContentUpdated,
                                                          {{"threadId", "wrong-thread"},
                                                           {"turnId", "turn-optional-content"},
                                                           {"itemId", "item-optional-content"},
                                                           {"channel", "agentText"},
                                                           {"content", "must not apply"}}};
        const auto decoded = model::decodeExpandedOccurrence(wrongParent, context());
        const auto reduced =
            decoded ? model::reduceOccurrence(snapshotWithItem(), decoded.value())
                    : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "decode failed"}};
        result.expectTrue(decoded && !reduced && reduced.error().path == "/threadId",
                          "item-content reduction rejects a stale or contradictory parent identity");
    }

    void testLegacyOnlyExpandedEncodingGuards(tests::support::TestResult& result) {
        struct GuardCase {
            frontend::FrontendEvent event;
            std::string_view path;
        };
        const std::array cases{
            GuardCase{{frontend::SequenceNumber{18}, "session.changed", {{"sessionId", "1"}, {"connected", true}, {"role", "observer"}}},
                      "/expandedPayloads/0/completeProjection"},
            GuardCase{{frontend::SequenceNumber{19},
                       "thread.updated",
                       {{"thread", {{"id", "thread-guard"}, {"fullyLoaded", true}, {"turns", frontend::Json::array()}}}}},
                      "/expandedPayloads/0/replaceDescendants"},
            GuardCase{{frontend::SequenceNumber{20},
                       "turn.updated",
                       {{"turn",
                         {{"id", "turn-guard"},
                          {"threadId", "thread-guard"},
                          {"status", "completed"},
                          {"active", false},
                          {"terminal", true},
                          {"items", frontend::Json::array()}}}}},
                      "/expandedPayloads/0/replaceItems"},
            GuardCase{{frontend::SequenceNumber{21},
                       "request.pending",
                       {{"request", {{"id", "21"}, {"type", "command_approval"}, {"details", frontend::Json::object()}}}}},
                      "/expandedPayloads/0/completeProjection"},
            GuardCase{{frontend::SequenceNumber{22},
                       "diagnostics.updated",
                       {{"received", std::uint64_t{1}}, {"recent", frontend::Json::array({"bounded"})}}},
                      "/expandedPayloads/0/aggregateLegacyUpdate"},
        };

        for (const GuardCase& guard : cases) {
            const auto decoded = model::decodeLegacyOccurrence(guard.event, context());
            const auto legacy = decoded ? model::encodeLegacyOccurrence(decoded.value())
                                        : model::OccurrenceResult<frontend::FrontendEvent>{decoded.error()};
            const auto expanded = decoded ? model::encodeExpandedOccurrence(decoded.value())
                                          : model::OccurrenceResult<std::vector<frontend::ExpandedFrontendEvent>>{decoded.error()};
            result.expectTrue(decoded && legacy && !expanded && expanded.error().code == model::OccurrenceErrorCode::EncodingFailure &&
                                  expanded.error().path == guard.path,
                              "lossy expanded-v1 conversion guard for " + guard.event.type + " at " + std::string(guard.path));
        }
    }

    void testServerCompatibilitySelection(tests::support::TestResult& result) {
        model::PendingRequestsUpdatedOccurrence pending;
        pending.pendingRequests.emplace_back(
            model::CommandExecutionApprovalRequest{model::PendingRequestData{model::PendingRequestIdentity{"31"}}});
        pending.pendingRequests.emplace_back(
            model::CommandExecutionApprovalRequest{model::PendingRequestData{model::PendingRequestIdentity{"32"}}});
        model::LegacyCompatibilityPayload pendingLegacy;
        pendingLegacy.kind = model::LegacyCompatibilityKind::PendingRequestAdded;
        model::OccurrenceIdentity pendingIdentity{model::FrontendSequence{23},
                                                  model::OccurrenceGroupIdentity{"pending-selection"},
                                                  0,
                                                  1,
                                                  model::SourceStamp{"backend:pending-added"}};
        pendingIdentity.pendingRequestId = model::PendingRequestIdentity{"32"};
        const auto pendingOccurrence = model::makeOccurrenceGroup(
            std::move(pendingIdentity), std::move(pendingLegacy), {model::OccurrencePayload{std::move(pending)}});
        const auto pendingExpanded = pendingOccurrence
                                         ? model::encodeExpandedOccurrence(pendingOccurrence.value())
                                         : model::OccurrenceResult<std::vector<frontend::ExpandedFrontendEvent>>{pendingOccurrence.error()};
        const auto pendingWire = pendingOccurrence ? model::encodeLegacyOccurrence(pendingOccurrence.value())
                                                   : model::OccurrenceResult<frontend::FrontendEvent>{pendingOccurrence.error()};

        model::SessionState observer{model::SessionIdentity{"41"}};
        model::SessionState controller{model::SessionIdentity{"42"}};
        controller.role = frontend::SessionRole::Controller;
        model::SessionsUpdatedOccurrence sessions{{std::move(observer), std::move(controller)}};
        sessions.connected = true;
        model::OccurrenceIdentity sessionIdentity{model::FrontendSequence{24},
                                                  model::OccurrenceGroupIdentity{"session-selection"},
                                                  0,
                                                  1,
                                                  model::SourceStamp{"backend:session-changed"}};
        sessionIdentity.sessionId = model::SessionIdentity{"42"};
        const auto sessionOccurrence = model::makeOccurrence(std::move(sessionIdentity), std::move(sessions));
        const auto sessionWire = sessionOccurrence ? model::encodeLegacyOccurrence(sessionOccurrence.value())
                                                   : model::OccurrenceResult<frontend::FrontendEvent>{sessionOccurrence.error()};

        model::DiagnosticRecord aggregateHead;
        aggregateHead.received = 2;
        model::DiagnosticsUpdatedOccurrence diagnostics{aggregateHead};
        model::DiagnosticRecord first;
        first.message = "first";
        model::DiagnosticRecord second;
        second.message = "second";
        diagnostics.aggregateEntries = {std::move(first), std::move(second)};
        const auto diagnosticOccurrence =
            model::makeOccurrence(model::OccurrenceIdentity{model::FrontendSequence{25},
                                                            model::OccurrenceGroupIdentity{"diagnostic-selection"},
                                                            0,
                                                            1,
                                                            model::SourceStamp{"backend:diagnostic"}},
                                  std::move(diagnostics));
        const auto diagnosticExpanded =
            diagnosticOccurrence ? model::encodeExpandedOccurrence(diagnosticOccurrence.value())
                                 : model::OccurrenceResult<std::vector<frontend::ExpandedFrontendEvent>>{diagnosticOccurrence.error()};
        const auto diagnosticLegacy = diagnosticOccurrence ? model::encodeLegacyOccurrence(diagnosticOccurrence.value())
                                                           : model::OccurrenceResult<frontend::FrontendEvent>{diagnosticOccurrence.error()};

        result.expectTrue(pendingExpanded && pendingExpanded.value().front().data.at("pendingRequests").size() == 2 && pendingWire &&
                              pendingWire.value().data.at("request").at("id") == "32" && sessionWire &&
                              sessionWire.value().data.at("sessionId") == "42" && sessionWire.value().data.at("role") == "controller" &&
                              sessionWire.value().data.at("connected") == true && diagnosticExpanded && diagnosticLegacy &&
                              diagnosticLegacy.value().data.at("recent") == frontend::Json::array({"first", "second"}),
                          "server-origin compatibility material selects exact identities while retaining full expanded projections");

        model::SessionsUpdatedOccurrence contradictorySessions{{model::SessionState{model::SessionIdentity{"42"}}}};
        model::LegacyCompatibilityPayload contradictorySessionLegacy;
        contradictorySessionLegacy.kind = model::LegacyCompatibilityKind::SessionChanged;
        contradictorySessionLegacy.changedSessionId = model::SessionIdentity{"42"};
        model::OccurrenceIdentity contradictorySessionIdentity{model::FrontendSequence{26},
                                                               model::OccurrenceGroupIdentity{"contradictory-session"},
                                                               0,
                                                               1,
                                                               model::SourceStamp{"backend:session-changed"}};
        contradictorySessionIdentity.sessionId = model::SessionIdentity{"41"};
        const auto contradictorySession = model::makeOccurrenceGroup(std::move(contradictorySessionIdentity),
                                                                     std::move(contradictorySessionLegacy),
                                                                     {model::OccurrencePayload{std::move(contradictorySessions)}});
        const auto contradictorySessionWire = contradictorySession
                                                  ? model::encodeLegacyOccurrence(contradictorySession.value())
                                                  : model::OccurrenceResult<frontend::FrontendEvent>{contradictorySession.error()};

        model::PendingRequestsUpdatedOccurrence contradictoryPending;
        contradictoryPending.removedRequestId = model::PendingRequestIdentity{"32"};
        model::LegacyCompatibilityPayload contradictoryPendingLegacy;
        contradictoryPendingLegacy.kind = model::LegacyCompatibilityKind::PendingRequestResolved;
        contradictoryPendingLegacy.resolvedRequestId = model::PendingRequestIdentity{"32"};
        model::OccurrenceIdentity contradictoryPendingIdentity{model::FrontendSequence{27},
                                                               model::OccurrenceGroupIdentity{"contradictory-pending"},
                                                               0,
                                                               1,
                                                               model::SourceStamp{"backend:request-resolved"}};
        contradictoryPendingIdentity.pendingRequestId = model::PendingRequestIdentity{"31"};
        const auto contradictoryPendingOccurrence = model::makeOccurrenceGroup(std::move(contradictoryPendingIdentity),
                                                                               std::move(contradictoryPendingLegacy),
                                                                               {model::OccurrencePayload{std::move(contradictoryPending)}});
        const auto contradictoryPendingWire =
            contradictoryPendingOccurrence ? model::encodeLegacyOccurrence(contradictoryPendingOccurrence.value())
                                           : model::OccurrenceResult<frontend::FrontendEvent>{contradictoryPendingOccurrence.error()};
        result.expectTrue(contradictorySession && !contradictorySessionWire &&
                              contradictorySessionWire.error().path == "/legacy/sessionId" && contradictoryPendingOccurrence &&
                              !contradictoryPendingWire && contradictoryPendingWire.error().path == "/legacy/pendingRequestId",
                          "canonical occurrence identities reject contradictory legacy session and pending-request selections");
    }
} // namespace

int main() {
    static_assert(std::variant_size_v<model::OccurrencePayload> == 26);
    tests::support::TestResult result;
    testLegacyPendingRoundTrips(result);
    testContainedExtension(result);
    testLegacyExtensionWireShape(result);
    testGeneratedMultiFamilyGroup(result);
    testItemContentPresence(result);
    testDiagnosticOccurrenceShape(result);
    testLegacyNestedOccurrenceRoundTrips(result);
    testLegacyPendingPresentationAndDiagnostics(result);
    testOccurrenceIdentityValidation(result);
    testLegacyOnlyExpandedEncodingGuards(result);
    testServerCompatibilitySelection(result);
    return result.processResult();
}
