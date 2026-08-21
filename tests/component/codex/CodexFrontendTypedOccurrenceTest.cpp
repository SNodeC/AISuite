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

    model::ThreadItem knownItem(std::string id,
                                std::size_t sourceIndex,
                                std::optional<std::string> summary = std::nullopt,
                                std::optional<model::TurnIdentity> turnId = std::nullopt) {
        model::ItemData data{model::ItemIdentity{std::move(id)}, std::nullopt, std::move(turnId)};
        data.sourceIndex = sourceIndex;
        data.summary = std::move(summary);
        return model::AgentMessageItem{std::move(data)};
    }

    model::LegacyItemCompatibility
    legacyItem(std::string id, std::size_t sourceIndex, std::optional<model::TurnIdentity> turnId = std::nullopt) {
        model::LegacyItemCompatibility item{
            model::ItemData{model::ItemIdentity{std::move(id)}, std::nullopt, std::move(turnId)},
            "future_item",
            sourceIndex,
            "/items/" + std::to_string(sourceIndex),
        };
        item.value.sourceIndex = sourceIndex;
        return item;
    }

    model::PendingRequest knownPending(std::string id, std::size_t sourceIndex, std::optional<std::string> summary = std::nullopt) {
        model::PendingRequestData data{model::PendingRequestIdentity{std::move(id)}};
        data.sourceIndex = sourceIndex;
        data.summary = std::move(summary);
        return model::CommandExecutionApprovalRequest{std::move(data)};
    }

    model::LegacyPendingRequestCompatibility legacyPending(std::string id, std::size_t sourceIndex) {
        model::LegacyPendingRequestCompatibility request{
            model::PendingRequestData{model::PendingRequestIdentity{std::move(id)}},
            sourceIndex,
            "/pendingRequests/" + std::to_string(sourceIndex),
        };
        request.value.sourceIndex = sourceIndex;
        return request;
    }

    std::vector<std::string> orderedItemIds(const model::CanonicalSnapshot& snapshot) {
        std::vector<std::pair<std::size_t, std::string>> ordered;
        for (const model::ThreadItem& item : snapshot.items) {
            const model::ItemData& data = model::itemData(item);
            ordered.emplace_back(data.sourceIndex.value_or(ordered.size()), data.id.value());
        }
        for (const model::LegacyItemCompatibility& item : snapshot.legacyItems) {
            ordered.emplace_back(item.sourceIndex, item.value.id.value());
        }
        std::stable_sort(ordered.begin(), ordered.end());
        std::vector<std::string> result;
        for (auto& [sourceIndex, id] : ordered) {
            (void) sourceIndex;
            result.push_back(std::move(id));
        }
        return result;
    }

    std::vector<std::string> orderedPendingIds(const model::CanonicalSnapshot& snapshot) {
        std::vector<std::pair<std::size_t, std::string>> ordered;
        for (const model::PendingRequest& request : snapshot.pendingRequests) {
            const model::PendingRequestData& data = model::pendingRequestData(request);
            ordered.emplace_back(data.sourceIndex.value_or(ordered.size()), data.id.value());
        }
        for (const model::LegacyPendingRequestCompatibility& request : snapshot.legacyPendingRequests) {
            ordered.emplace_back(request.sourceIndex, request.value.id.value());
        }
        std::stable_sort(ordered.begin(), ordered.end());
        std::vector<std::string> result;
        for (auto& [sourceIndex, id] : ordered) {
            (void) sourceIndex;
            result.push_back(std::move(id));
        }
        return result;
    }

    model::OccurrenceIdentity occurrenceIdentity(std::uint64_t sequence, std::string group) {
        return {model::FrontendSequence{sequence},
                model::OccurrenceGroupIdentity{std::move(group)},
                0,
                1,
                model::SourceStamp{"typed-occurrence-ordering"}};
    }

    void testInPlaceAndCopyPreservingReduction(tests::support::TestResult& result) {
        model::CanonicalSnapshot candidate = snapshotWithItem();
        model::ItemData unchanged{model::ItemIdentity{"unchanged-item"},
                                  model::ThreadIdentity{"thread-optional-content"},
                                  model::TurnIdentity{"turn-optional-content"}};
        unchanged.agentText = "unchanged";
        candidate.items.emplace_back(model::AgentMessageItem{std::move(unchanged)});
        const model::CanonicalSnapshot source = candidate;
        const model::CanonicalSnapshot sourceBefore = source;

        model::ItemContentUpdatedOccurrence content{model::ItemIdentity{"item-optional-content"}};
        content.threadId = model::ThreadIdentity{"thread-optional-content"};
        content.turnId = model::TurnIdentity{"turn-optional-content"};
        content.channel = "agentText";
        content.content = "after";
        auto identity = occurrenceIdentity(31, "in-place-item-content");
        identity.threadId = content.threadId;
        identity.turnId = content.turnId;
        identity.itemId = content.itemId;
        const auto occurrence = model::makeOccurrence(std::move(identity), std::move(content));
        if (!occurrence) {
            result.expectTrue(false, "in-place occurrence reduction mutates the caller-owned candidate");
            result.expectTrue(false, "copy-preserving occurrence reduction leaves its source unchanged");
            return;
        }

        const auto applied = model::applyOccurrence(candidate, occurrence.value());
        result.expectTrue(applied && model::itemData(std::as_const(candidate.items).front()).agentText ==
                                         std::optional<std::string>{"after"} &&
                              model::itemData(std::as_const(candidate.items).back()).agentText ==
                                  std::optional<std::string>{"unchanged"},
                          "in-place occurrence reduction mutates only the target while retaining unrelated item values");

        const auto reduced = model::reduceOccurrence(source, occurrence.value());
        result.expectTrue(reduced && source == sourceBefore &&
                              model::itemData(source.items.front()).agentText == std::optional<std::string>{"before"} &&
                              model::itemData(reduced.value().items.front()).agentText == std::optional<std::string>{"after"},
                          "copy-preserving occurrence reduction leaves its source unchanged");
    }

    void testItemOrderingAndAuthorityMigration(tests::support::TestResult& result) {
        model::CanonicalSnapshot state;
        state.items.push_back(knownItem("first", 0));
        state.items.push_back(knownItem("middle", 1));
        state.legacyItems.push_back(legacyItem("last", 2));

        auto middleIdentity = occurrenceIdentity(32, "known-middle-update");
        middleIdentity.itemId = model::ItemIdentity{"middle"};
        const auto middleOccurrence =
            model::makeOccurrence(std::move(middleIdentity), model::ItemUpsertedOccurrence{knownItem("middle", 0, "updated in place")});
        auto middleState =
            middleOccurrence
                ? model::reduceOccurrence(state, middleOccurrence.value())
                : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        if (!middleState) {
            result.expectTrue(false, "known item update preserves its logical source slot");
            return;
        }
        state = std::move(middleState).value();

        auto appendIdentity = occurrenceIdentity(33, "known-item-append");
        appendIdentity.itemId = model::ItemIdentity{"appended"};
        const auto appendOccurrence =
            model::makeOccurrence(std::move(appendIdentity), model::ItemUpsertedOccurrence{knownItem("appended", 0)});
        auto appendedState =
            appendOccurrence
                ? model::reduceOccurrence(state, appendOccurrence.value())
                : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        if (!appendedState) {
            result.expectTrue(false, "new item occurrence appends after the retained mixed sequence");
            return;
        }
        state = std::move(appendedState).value();

        model::LegacyCompatibilityPayload middleLegacy;
        middleLegacy.kind = model::LegacyCompatibilityKind::LegacyItem;
        middleLegacy.legacyItem = legacyItem("middle", 0);
        auto legacyIdentity = occurrenceIdentity(34, "known-to-legacy-item");
        legacyIdentity.itemId = model::ItemIdentity{"middle"};
        const auto legacyOccurrence = model::makeOccurrenceGroup(std::move(legacyIdentity), std::move(middleLegacy), {});
        auto legacyState =
            legacyOccurrence
                ? model::reduceOccurrence(state, legacyOccurrence.value())
                : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        if (!legacyState) {
            result.expectTrue(false, "known-to-legacy item migration retains one authority and one source slot");
            return;
        }
        state = std::move(legacyState).value();

        auto knownIdentity = occurrenceIdentity(35, "legacy-to-known-item");
        knownIdentity.itemId = model::ItemIdentity{"last"};
        const auto knownOccurrence =
            model::makeOccurrence(std::move(knownIdentity), model::ItemUpsertedOccurrence{knownItem("last", 0, "now known")});
        auto knownState =
            knownOccurrence
                ? model::reduceOccurrence(state, knownOccurrence.value())
                : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        if (!knownState) {
            result.expectTrue(false, "legacy-to-known item migration retains one authority and one source slot");
            return;
        }
        state = std::move(knownState).value();

        const auto knownMiddle = std::find_if(state.items.begin(), state.items.end(), [](const model::ThreadItem& item) {
            return model::itemData(item).id == model::ItemIdentity{"middle"};
        });
        const auto legacyMiddle = std::find_if(state.legacyItems.begin(), state.legacyItems.end(), [](const auto& item) {
            return item.value.id == model::ItemIdentity{"middle"};
        });
        const auto knownLast = std::find_if(state.items.begin(), state.items.end(), [](const model::ThreadItem& item) {
            return model::itemData(item).id == model::ItemIdentity{"last"};
        });
        result.expectTrue(orderedItemIds(state) == std::vector<std::string>({"first", "middle", "last", "appended"}) &&
                              knownMiddle == state.items.end() && legacyMiddle != state.legacyItems.end() &&
                              legacyMiddle->sourceIndex == 1 && legacyMiddle->value.sourceIndex == std::optional<std::size_t>{1} &&
                              knownLast != state.items.end() && model::itemData(*knownLast).sourceIndex == std::optional<std::size_t>{2} &&
                              state.legacyItems.size() == 1,
                          "item upserts preserve mixed ordering, append new identities, and migrate authority without duplicates");
    }

    void testScopedItemIdentityReduction(tests::support::TestResult& result) {
        model::CanonicalSnapshot state;
        model::ItemData first{model::ItemIdentity{"item-1"},
                              model::ThreadIdentity{"first-thread"},
                              model::TurnIdentity{"first-turn"}};
        first.agentText = "first";
        state.items.emplace_back(model::AgentMessageItem{std::move(first)});
        model::ItemData second{model::ItemIdentity{"item-1"},
                               model::ThreadIdentity{"second-thread"},
                               model::TurnIdentity{"second-turn"}};
        second.agentText = "second";
        state.items.emplace_back(model::AgentMessageItem{std::move(second)});

        model::ItemData replacement{model::ItemIdentity{"item-1"},
                                    model::ThreadIdentity{"second-thread"},
                                    model::TurnIdentity{"second-turn"}};
        replacement.agentText = "second replaced";
        auto upsertIdentity = occurrenceIdentity(36, "scoped-item-upsert");
        upsertIdentity.threadId = model::ThreadIdentity{"second-thread"};
        upsertIdentity.turnId = model::TurnIdentity{"second-turn"};
        upsertIdentity.itemId = model::ItemIdentity{"item-1"};
        const auto upsert = model::makeOccurrence(
            std::move(upsertIdentity), model::ItemUpsertedOccurrence{model::AgentMessageItem{std::move(replacement)}});
        const auto upserted = upsert ? model::reduceOccurrence(state, upsert.value())
                                    : model::ModelResult<model::CanonicalSnapshot>{
                                          {model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};

        model::ItemContentUpdatedOccurrence content{model::ItemIdentity{"item-1"}};
        content.threadId = model::ThreadIdentity{"second-thread"};
        content.turnId = model::TurnIdentity{"second-turn"};
        content.channel = "agentText";
        content.content = "second updated";
        auto contentIdentity = occurrenceIdentity(37, "scoped-item-content");
        contentIdentity.threadId = content.threadId;
        contentIdentity.turnId = content.turnId;
        contentIdentity.itemId = content.itemId;
        const auto contentOccurrence = model::makeOccurrence(std::move(contentIdentity), std::move(content));
        const auto updated = upserted && contentOccurrence
                                 ? model::reduceOccurrence(upserted.value(), contentOccurrence.value())
                                 : model::ModelResult<model::CanonicalSnapshot>{
                                       {model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        const auto findItem = [](const model::CanonicalSnapshot& snapshot, std::string_view threadId) {
            return std::find_if(snapshot.items.begin(), snapshot.items.end(), [&](const model::ThreadItem& item) {
                const model::ItemData& data = model::itemData(item);
                return data.id == model::ItemIdentity{"item-1"} &&
                       data.threadId == std::optional<model::ThreadIdentity>{model::ThreadIdentity{std::string(threadId)}};
            });
        };
        const model::ThreadItem* firstItem = nullptr;
        const model::ThreadItem* secondItem = nullptr;
        if (updated) {
            const auto first = findItem(updated.value(), "first-thread");
            const auto second = findItem(updated.value(), "second-thread");
            firstItem = first == updated.value().items.end() ? nullptr : &*first;
            secondItem = second == updated.value().items.end() ? nullptr : &*second;
        }
        result.expectTrue(updated && updated.value().items.size() == 2 && firstItem && secondItem &&
                              model::itemData(*firstItem).agentText == "first" &&
                              model::itemData(*secondItem).agentText == "second updated",
                          "item upsert and content occurrences target the exact thread/turn/item identity when provider IDs repeat");
    }

    model::CanonicalSnapshot forkedTurnSnapshot() {
        model::CanonicalSnapshot snapshot;
        const model::ThreadIdentity firstThread{"first-thread"};
        const model::ThreadIdentity secondThread{"second-thread"};
        const model::TurnIdentity sharedTurn{"shared-turn"};
        snapshot.threads.emplace_back(firstThread);
        snapshot.threads.emplace_back(secondThread);
        snapshot.turns.emplace_back(sharedTurn, firstThread);
        snapshot.turns.emplace_back(sharedTurn, secondThread);
        snapshot.items.emplace_back(model::AgentMessageItem{
            model::ItemData{model::ItemIdentity{"first-item"}, firstThread, sharedTurn}});
        snapshot.items.emplace_back(model::AgentMessageItem{
            model::ItemData{model::ItemIdentity{"second-item"}, secondThread, sharedTurn}});
        snapshot.items.emplace_back(model::AgentMessageItem{
            model::ItemData{model::ItemIdentity{"ambiguous-unscoped"}, std::nullopt, sharedTurn}});
        return snapshot;
    }

    model::CanonicalSnapshot partialForkTurnSnapshot(bool legacyScopeEvidence) {
        model::CanonicalSnapshot snapshot;
        const model::ThreadIdentity retainedThread{"retained-thread"};
        const model::ThreadIdentity omittedThread{"omitted-thread"};
        const model::TurnIdentity sharedTurn{"partial-shared-turn"};
        snapshot.threads.emplace_back(retainedThread);
        snapshot.threads.emplace_back(omittedThread);
        snapshot.turns.emplace_back(sharedTurn, retainedThread);
        snapshot.items.emplace_back(model::AgentMessageItem{
            model::ItemData{model::ItemIdentity{"retained-scoped"}, retainedThread, sharedTurn}});
        snapshot.items.emplace_back(model::AgentMessageItem{
            model::ItemData{model::ItemIdentity{"ambiguous-unscoped"}, std::nullopt, sharedTurn}});
        if (legacyScopeEvidence) {
            snapshot.legacyItems.push_back({model::ItemData{model::ItemIdentity{"omitted-scoped"}, omittedThread, sharedTurn},
                                            "future_item",
                                            2,
                                            "/items/2"});
        } else {
            snapshot.items.emplace_back(model::AgentMessageItem{
                model::ItemData{model::ItemIdentity{"omitted-scoped"}, omittedThread, sharedTurn}});
        }
        return snapshot;
    }

    bool containsItem(const model::CanonicalSnapshot& snapshot, std::string_view itemId) {
        return std::ranges::any_of(snapshot.items, [itemId](const model::ThreadItem& item) {
            return model::itemData(item).id.value() == itemId;
        });
    }

    void testForkTurnDescendantScoping(tests::support::TestResult& result) {
        const model::CanonicalSnapshot source = forkedTurnSnapshot();

        auto removedIdentity = occurrenceIdentity(38, "remove-fork-parent");
        removedIdentity.threadId = model::ThreadIdentity{"first-thread"};
        const auto removedOccurrence = model::makeOccurrence(
            std::move(removedIdentity), model::ThreadRemovedOccurrence{model::ThreadIdentity{"first-thread"}});
        const auto removed = removedOccurrence
                                 ? model::reduceOccurrence(source, removedOccurrence.value())
                                 : model::ModelResult<model::CanonicalSnapshot>{
                                       {model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        result.expectTrue(removed && removed.value().turns.size() == 1 && removed.value().items.size() == 2 &&
                              removed.value().turns.front().threadId.value() == "second-thread" &&
                              containsItem(removed.value(), "second-item") &&
                              containsItem(removed.value(), "ambiguous-unscoped"),
                          "removing a fork parent preserves sibling and ambiguous unscoped descendants with the same turn ID");

        model::ThreadUpsertedOccurrence threadUpdate{model::ThreadState{model::ThreadIdentity{"first-thread"}}};
        threadUpdate.replaceDescendants = true;
        threadUpdate.turns.emplace_back(model::TurnIdentity{"shared-turn"}, model::ThreadIdentity{"first-thread"});
        threadUpdate.items.emplace_back(model::AgentMessageItem{
            model::ItemData{model::ItemIdentity{"first-replacement"},
                            model::ThreadIdentity{"first-thread"},
                            model::TurnIdentity{"shared-turn"}}});
        auto threadIdentity = occurrenceIdentity(39, "replace-fork-thread");
        threadIdentity.threadId = model::ThreadIdentity{"first-thread"};
        const auto threadOccurrence = model::makeOccurrence(std::move(threadIdentity), std::move(threadUpdate));
        const auto threadReplaced = threadOccurrence
                                        ? model::reduceOccurrence(source, threadOccurrence.value())
                                        : model::ModelResult<model::CanonicalSnapshot>{
                                              {model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        const auto secondAfterThreadReplace = threadReplaced
                                                  ? std::find_if(threadReplaced.value().items.begin(),
                                                                 threadReplaced.value().items.end(),
                                                                 [](const model::ThreadItem& item) {
                                                                     return model::itemData(item).id.value() == "second-item";
                                                                 })
                                                  : source.items.end();
        result.expectTrue(threadReplaced && threadReplaced.value().turns.size() == 2 &&
                              threadReplaced.value().items.size() == 3 &&
                              secondAfterThreadReplace != threadReplaced.value().items.end() &&
                              model::itemData(*secondAfterThreadReplace).threadId ==
                                  std::optional<model::ThreadIdentity>{model::ThreadIdentity{"second-thread"}} &&
                              containsItem(threadReplaced.value(), "ambiguous-unscoped"),
                          "replacing one thread's descendants preserves a fork sibling with the same turn ID");

        model::TurnUpsertedOccurrence turnUpdate{
            model::TurnState{model::TurnIdentity{"shared-turn"}, model::ThreadIdentity{"first-thread"}}};
        turnUpdate.replaceItems = true;
        turnUpdate.items.emplace_back(model::AgentMessageItem{
            model::ItemData{model::ItemIdentity{"first-turn-replacement"},
                            model::ThreadIdentity{"first-thread"},
                            model::TurnIdentity{"shared-turn"}}});
        auto turnIdentity = occurrenceIdentity(40, "replace-fork-turn");
        turnIdentity.threadId = model::ThreadIdentity{"first-thread"};
        turnIdentity.turnId = model::TurnIdentity{"shared-turn"};
        const auto turnOccurrence = model::makeOccurrence(std::move(turnIdentity), std::move(turnUpdate));
        const auto turnReplaced = turnOccurrence
                                      ? model::reduceOccurrence(source, turnOccurrence.value())
                                      : model::ModelResult<model::CanonicalSnapshot>{
                                            {model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        const auto secondAfterTurnReplace = turnReplaced
                                                ? std::find_if(turnReplaced.value().items.begin(),
                                                               turnReplaced.value().items.end(),
                                                               [](const model::ThreadItem& item) {
                                                                   return model::itemData(item).id.value() == "second-item";
                                                               })
                                                : source.items.end();
        result.expectTrue(turnReplaced && turnReplaced.value().turns.size() == 2 && turnReplaced.value().items.size() == 3 &&
                              secondAfterTurnReplace != turnReplaced.value().items.end() &&
                              model::itemData(*secondAfterTurnReplace).threadId ==
                                  std::optional<model::ThreadIdentity>{model::ThreadIdentity{"second-thread"}} &&
                              containsItem(turnReplaced.value(), "ambiguous-unscoped"),
                          "replacing one fork turn's items preserves a sibling thread's same-ID turn items");

        const model::CanonicalSnapshot partialSource = partialForkTurnSnapshot(false);
        auto partialRemovedIdentity = occurrenceIdentity(43, "remove-partial-fork-parent");
        partialRemovedIdentity.threadId = model::ThreadIdentity{"retained-thread"};
        const auto partialRemovedOccurrence = model::makeOccurrence(
            std::move(partialRemovedIdentity), model::ThreadRemovedOccurrence{model::ThreadIdentity{"retained-thread"}});
        const auto partialRemoved = partialRemovedOccurrence
                                        ? model::reduceOccurrence(partialSource, partialRemovedOccurrence.value())
                                        : model::ModelResult<model::CanonicalSnapshot>{
                                              {model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};

        model::ThreadUpsertedOccurrence partialThreadUpdate{
            model::ThreadState{model::ThreadIdentity{"retained-thread"}}};
        partialThreadUpdate.replaceDescendants = true;
        partialThreadUpdate.turns.emplace_back(model::TurnIdentity{"partial-shared-turn"},
                                               model::ThreadIdentity{"retained-thread"});
        partialThreadUpdate.items.emplace_back(model::AgentMessageItem{
            model::ItemData{model::ItemIdentity{"thread-replacement"},
                            model::ThreadIdentity{"retained-thread"},
                            model::TurnIdentity{"partial-shared-turn"}}});
        auto partialThreadIdentity = occurrenceIdentity(44, "replace-partial-fork-thread");
        partialThreadIdentity.threadId = model::ThreadIdentity{"retained-thread"};
        const auto partialThreadOccurrence =
            model::makeOccurrence(std::move(partialThreadIdentity), std::move(partialThreadUpdate));
        const auto partialThreadReplaced = partialThreadOccurrence
                                               ? model::reduceOccurrence(partialSource, partialThreadOccurrence.value())
                                               : model::ModelResult<model::CanonicalSnapshot>{
                                                     {model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};

        model::TurnUpsertedOccurrence partialTurnUpdate{
            model::TurnState{model::TurnIdentity{"partial-shared-turn"}, model::ThreadIdentity{"retained-thread"}}};
        partialTurnUpdate.replaceItems = true;
        partialTurnUpdate.items.emplace_back(model::AgentMessageItem{
            model::ItemData{model::ItemIdentity{"turn-replacement"},
                            model::ThreadIdentity{"retained-thread"},
                            model::TurnIdentity{"partial-shared-turn"}}});
        auto partialTurnIdentity = occurrenceIdentity(45, "replace-partial-fork-turn");
        partialTurnIdentity.threadId = model::ThreadIdentity{"retained-thread"};
        partialTurnIdentity.turnId = model::TurnIdentity{"partial-shared-turn"};
        const auto partialTurnOccurrence =
            model::makeOccurrence(std::move(partialTurnIdentity), std::move(partialTurnUpdate));
        const auto partialTurnReplaced = partialTurnOccurrence
                                             ? model::reduceOccurrence(partialSource, partialTurnOccurrence.value())
                                             : model::ModelResult<model::CanonicalSnapshot>{
                                                   {model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        result.expectTrue(partialRemoved && containsItem(partialRemoved.value(), "ambiguous-unscoped") &&
                              partialThreadReplaced && containsItem(partialThreadReplaced.value(), "ambiguous-unscoped") &&
                              partialTurnReplaced && containsItem(partialTurnReplaced.value(), "ambiguous-unscoped"),
                          "partial fork reductions preserve ambiguous unscoped descendants for every replacement path");

        const model::CanonicalSnapshot partialLegacySource = partialForkTurnSnapshot(true);
        auto legacyRemovedIdentity = occurrenceIdentity(46, "remove-partial-legacy-fork-parent");
        legacyRemovedIdentity.threadId = model::ThreadIdentity{"retained-thread"};
        const auto legacyRemovedOccurrence = model::makeOccurrence(
            std::move(legacyRemovedIdentity), model::ThreadRemovedOccurrence{model::ThreadIdentity{"retained-thread"}});
        const auto legacyRemoved = legacyRemovedOccurrence
                                       ? model::reduceOccurrence(partialLegacySource, legacyRemovedOccurrence.value())
                                       : model::ModelResult<model::CanonicalSnapshot>{
                                             {model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        result.expectTrue(legacyRemoved && containsItem(legacyRemoved.value(), "ambiguous-unscoped"),
                          "partial fork reduction also treats scoped legacy items as ambiguity evidence");

        model::ThreadUpsertedOccurrence wireUpdate{model::ThreadState{model::ThreadIdentity{"first-thread"}}};
        wireUpdate.turns.emplace_back(model::TurnIdentity{"shared-turn"}, model::ThreadIdentity{"first-thread"});
        wireUpdate.items.assign(source.items.begin(), source.items.end());
        auto wireIdentity = occurrenceIdentity(41, "encode-fork-thread");
        wireIdentity.threadId = model::ThreadIdentity{"first-thread"};
        const auto wireOccurrence = model::makeOccurrence(std::move(wireIdentity), std::move(wireUpdate));
        const auto legacy = wireOccurrence ? model::encodeLegacyOccurrence(wireOccurrence.value())
                                           : model::OccurrenceResult<frontend::FrontendEvent>{wireOccurrence.error()};
        const frontend::Json nestedItems = legacy ? legacy.value().data.at("thread").at("turns").at(0).at("items")
                                                  : frontend::Json::array();
        result.expectTrue(legacy && nestedItems.size() == 1 && nestedItems.at(0).value("id", "") == "first-item",
                          "legacy thread occurrences nest same-ID fork turns and items under the exact parent thread");

        model::ThreadUpsertedOccurrence uniqueWireUpdate{
            model::ThreadState{model::ThreadIdentity{"unique-thread"}}};
        uniqueWireUpdate.turns.emplace_back(model::TurnIdentity{"unique-turn"}, model::ThreadIdentity{"unique-thread"});
        uniqueWireUpdate.items.emplace_back(model::AgentMessageItem{
            model::ItemData{model::ItemIdentity{"unique-unscoped"},
                            std::nullopt,
                            model::TurnIdentity{"unique-turn"}}});
        auto uniqueWireIdentity = occurrenceIdentity(42, "encode-unique-unscoped-thread");
        uniqueWireIdentity.threadId = model::ThreadIdentity{"unique-thread"};
        const auto uniqueWireOccurrence =
            model::makeOccurrence(std::move(uniqueWireIdentity), std::move(uniqueWireUpdate));
        const auto uniqueLegacy = uniqueWireOccurrence
                                      ? model::encodeLegacyOccurrence(uniqueWireOccurrence.value())
                                      : model::OccurrenceResult<frontend::FrontendEvent>{uniqueWireOccurrence.error()};
        const frontend::Json uniqueNestedItems = uniqueLegacy
                                                     ? uniqueLegacy.value().data.at("thread").at("turns").at(0).at("items")
                                                     : frontend::Json::array();
        result.expectTrue(uniqueLegacy && uniqueNestedItems.size() == 1 &&
                              uniqueNestedItems.at(0).value("id", "") == "unique-unscoped",
                          "legacy thread occurrences preserve a turn-only item when its turn identity is unambiguous");
    }

    void testUnicodeItemContentReduction(tests::support::TestResult& result) {
        const std::string content = std::string(16'383, 'x') + "\xE2\x82\xAC";
        const frontend::ExpandedFrontendEvent event{
            frontend::SequenceNumber{38},
            frontend::ExpandedEventType::ItemContentUpdated,
            {{"threadId", "thread-optional-content"},
             {"turnId", "turn-optional-content"},
             {"itemId", "item-optional-content"},
             {"channel", "agentText"},
             {"content", content},
             {"contentTruncated", false},
             {"droppedContentBytes", std::uint64_t{0}}}};
        const auto decoded = model::decodeExpandedOccurrence(event, context());
        const auto reduced =
            decoded ? model::reduceOccurrence(snapshotWithItem(), decoded.value())
                    : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "decode failed"}};
        const model::ItemData* item =
            reduced && reduced.value().items.size() == 1 ? &model::itemData(reduced.value().items.front()) : nullptr;
        result.expectTrue(decoded && item && item->agentText == content && !item->contentTruncated &&
                              item->droppedContentBytes == std::optional<std::uint64_t>{0},
                          "client reduction preserves 16,384 Unicode characters even when their UTF-8 encoding exceeds 16 KiB");
    }

    void testPendingOrderingAndAuthorityMigration(tests::support::TestResult& result) {
        model::CanonicalSnapshot state;
        state.pendingRequests.push_back(knownPending("first", 0));
        state.pendingRequests.push_back(knownPending("middle", 1));
        state.legacyPendingRequests.push_back(legacyPending("last", 2));

        const auto reduceKnown = [&](std::uint64_t sequence, model::PendingRequest request, std::string group) {
            model::PendingRequestsUpdatedOccurrence update;
            update.completeProjection = false;
            update.pendingRequests.push_back(std::move(request));
            auto identity = occurrenceIdentity(sequence, std::move(group));
            identity.pendingRequestId = model::pendingRequestData(update.pendingRequests.front()).id;
            const auto occurrence = model::makeOccurrence(std::move(identity), std::move(update));
            return occurrence
                       ? model::reduceOccurrence(state, occurrence.value())
                       : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        };

        auto middleState = reduceKnown(36, knownPending("middle", 0, "updated in place"), "known-pending-update");
        if (!middleState) {
            result.expectTrue(false, "known pending-request update preserves its logical source slot");
            return;
        }
        state = std::move(middleState).value();
        auto appendedState = reduceKnown(37, knownPending("appended", 0), "known-pending-append");
        if (!appendedState) {
            result.expectTrue(false, "new pending-request occurrence appends after the retained mixed sequence");
            return;
        }
        state = std::move(appendedState).value();

        model::LegacyCompatibilityPayload middleLegacy;
        middleLegacy.kind = model::LegacyCompatibilityKind::LegacyPendingRequest;
        middleLegacy.legacyPendingRequest = legacyPending("middle", 0);
        auto legacyIdentity = occurrenceIdentity(38, "known-to-legacy-pending");
        legacyIdentity.pendingRequestId = model::PendingRequestIdentity{"middle"};
        const auto legacyOccurrence = model::makeOccurrenceGroup(std::move(legacyIdentity), std::move(middleLegacy), {});
        auto legacyState =
            legacyOccurrence
                ? model::reduceOccurrence(state, legacyOccurrence.value())
                : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        if (!legacyState) {
            result.expectTrue(false, "known-to-legacy pending migration retains one authority and one source slot");
            return;
        }
        state = std::move(legacyState).value();
        auto knownState = reduceKnown(39, knownPending("last", 0, "now known"), "legacy-to-known-pending");
        if (!knownState) {
            result.expectTrue(false, "legacy-to-known pending migration retains one authority and one source slot");
            return;
        }
        state = std::move(knownState).value();

        model::PendingRequestsUpdatedOccurrence remove;
        remove.completeProjection = false;
        remove.removedRequestId = model::PendingRequestIdentity{"first"};
        auto removeIdentity = occurrenceIdentity(40, "pending-remove-reindex");
        removeIdentity.pendingRequestId = *remove.removedRequestId;
        const auto removeOccurrence = model::makeOccurrence(std::move(removeIdentity), std::move(remove));
        auto removedState =
            removeOccurrence
                ? model::reduceOccurrence(state, removeOccurrence.value())
                : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        if (!removedState) {
            result.expectTrue(false, "pending-request removal closes the mixed source-order gap");
            return;
        }
        state = std::move(removedState).value();

        const auto knownMiddle = std::find_if(state.pendingRequests.begin(), state.pendingRequests.end(), [](const auto& request) {
            return model::pendingRequestData(request).id == model::PendingRequestIdentity{"middle"};
        });
        const auto legacyMiddle =
            std::find_if(state.legacyPendingRequests.begin(), state.legacyPendingRequests.end(), [](const auto& request) {
                return request.value.id == model::PendingRequestIdentity{"middle"};
            });
        const auto knownLast = std::find_if(state.pendingRequests.begin(), state.pendingRequests.end(), [](const auto& request) {
            return model::pendingRequestData(request).id == model::PendingRequestIdentity{"last"};
        });
        result.expectTrue(orderedPendingIds(state) == std::vector<std::string>({"middle", "last", "appended"}) &&
                              knownMiddle == state.pendingRequests.end() && legacyMiddle != state.legacyPendingRequests.end() &&
                              legacyMiddle->sourceIndex == 0 && legacyMiddle->value.sourceIndex == std::optional<std::size_t>{0} &&
                              knownLast != state.pendingRequests.end() &&
                              model::pendingRequestData(*knownLast).sourceIndex == std::optional<std::size_t>{1} &&
                              state.legacyPendingRequests.size() == 1,
                          "pending upserts preserve mixed ordering, append, migrate authority once, and reindex after removal");
    }

    void testDescendantReplacementOrdering(tests::support::TestResult& result) {
        const model::TurnIdentity targetTurn{"target-turn"};
        model::CanonicalSnapshot state;
        state.turns.emplace_back(targetTurn, model::ThreadIdentity{"thread"});
        state.items.push_back(knownItem("before", 0, std::nullopt, model::TurnIdentity{"before-turn"}));
        state.items.push_back(knownItem("target-known", 1, std::nullopt, targetTurn));
        state.items.push_back(knownItem("between", 2, std::nullopt, model::TurnIdentity{"between-turn"}));
        state.legacyItems.push_back(legacyItem("target-legacy", 3, targetTurn));
        state.items.push_back(knownItem("after", 4, std::nullopt, model::TurnIdentity{"after-turn"}));

        model::TurnUpsertedOccurrence update{model::TurnState{targetTurn, model::ThreadIdentity{"thread"}}};
        update.replaceItems = true;
        update.items.push_back(knownItem("replacement", 0, std::nullopt, targetTurn));
        auto identity = occurrenceIdentity(41, "turn-descendant-order");
        identity.turnId = targetTurn;
        const auto occurrence = model::makeOccurrence(std::move(identity), std::move(update));
        const auto reduced =
            occurrence ? model::reduceOccurrence(state, occurrence.value())
                       : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        result.expectTrue(reduced &&
                              orderedItemIds(reduced.value()) == std::vector<std::string>({"before", "replacement", "between", "after"}) &&
                              reduced.value().legacyItems.empty(),
                          "complete turn replacement inserts descendants at the first affected mixed item slot");
    }

    void testTurnPlanOccurrenceRoundTrip(tests::support::TestResult& result) {
        const model::ThreadIdentity threadId{"plan-thread"};
        const model::TurnIdentity turnId{"plan-turn"};
        model::TurnState turn{turnId, threadId};
        turn.plan = model::TurnPlanState{"Dependency order",
                                         {{"Inspect", "completed"}, {"Implement", "inProgress"}, {"Verify", "pending"}},
                                         3,
                                         false};
        model::TurnUpsertedOccurrence update{std::move(turn)};
        auto identity = occurrenceIdentity(42, "turn-plan-round-trip");
        identity.threadId = threadId;
        identity.turnId = turnId;
        const auto occurrence = model::makeOccurrence(std::move(identity), std::move(update));
        const auto encoded = occurrence
                                 ? model::encodeExpandedOccurrence(occurrence.value())
                                 : model::OccurrenceResult<std::vector<frontend::ExpandedFrontendEvent>>{occurrence.error()};
        const model::OccurrenceDecodeContext planContext{
            model::OccurrenceGroupIdentity{"turn-plan-round-trip"},
            0,
            1,
            model::SourceStamp{"server_notification:ServerNotification:method:turn/plan/updated"}};
        const auto decoded = encoded
                                 ? model::decodeExpandedOccurrence(encoded.value().front(), planContext)
                                 : model::OccurrenceResult<model::CanonicalOccurrence>{encoded.error()};
        model::CanonicalSnapshot initial;
        initial.threads.emplace_back(threadId);
        const auto reduced = decoded
                                 ? model::reduceOccurrence(initial, decoded.value())
                                 : model::ModelResult<model::CanonicalSnapshot>{
                                       {model::ModelErrorCode::InvalidShape, "/event", "decode failed"}};
        const std::optional<model::TurnPlanState> retainedPlan =
            reduced && reduced.value().turns.size() == 1 ? reduced.value().turns.front().plan : std::nullopt;
        const std::vector<model::TurnPlanStepState> expectedSteps{
            {"Inspect", "completed"}, {"Implement", "inProgress"}, {"Verify", "pending"}};
        result.expectTrue(encoded && encoded.value().size() == 1 &&
                              encoded.value().front().type == frontend::ExpandedEventType::TurnUpserted && decoded && reduced &&
                              retainedPlan && retainedPlan->explanation == std::optional<std::string>{"Dependency order"} &&
                              retainedPlan->steps == expectedSteps &&
                              retainedPlan->totalSteps == 3 && !retainedPlan->truncated,
                          "turn.upserted preserves ordered typed-plan projection through wire decode and canonical reduction");
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

    void testNegotiatedItemContentAppend(tests::support::TestResult& result) {
        model::ItemContentUpdatedOccurrence update{model::ItemIdentity{"item-optional-content"}};
        update.threadId = model::ThreadIdentity{"thread-optional-content"};
        update.turnId = model::TurnIdentity{"turn-optional-content"};
        update.channel = "agentText";
        update.content = "before after";
        update.appendHint = model::ItemContentAppendHint{6, " after"};
        update.truncation = {};
        const auto occurrence = model::makeOccurrence(occurrenceIdentity(11, "append-content"), std::move(update));
        const auto replacementWire = occurrence
                                         ? model::encodeExpandedOccurrence(occurrence.value())
                                         : model::OccurrenceResult<std::vector<frontend::ExpandedFrontendEvent>>{occurrence.error()};
        const auto appendWire = occurrence
                                    ? model::encodeExpandedOccurrence(occurrence.value(), model::ItemContentWireMode::AppendV1)
                                    : model::OccurrenceResult<std::vector<frontend::ExpandedFrontendEvent>>{occurrence.error()};
        const auto legacyWire = occurrence ? model::encodeLegacyOccurrence(occurrence.value())
                                           : model::OccurrenceResult<frontend::FrontendEvent>{occurrence.error()};
        const auto decoded = appendWire ? model::decodeExpandedOccurrence(
                                              appendWire.value().front(), context(), model::ItemContentWireMode::AppendV1)
                                        : model::OccurrenceResult<model::CanonicalOccurrence>{appendWire.error()};
        const auto reduced = decoded
                                 ? model::reduceOccurrence(snapshotWithItem(), decoded.value())
                                 : model::ModelResult<model::CanonicalSnapshot>{
                                       {model::ModelErrorCode::InvalidShape, "/event", "decode failed"}};
        const auto* decodedUpdate = decoded && !decoded.value().expandedPayloads().empty()
                                        ? std::get_if<model::ItemContentUpdatedOccurrence>(
                                              &decoded.value().expandedPayloads().front())
                                        : nullptr;
        const model::ItemData* reducedItem =
            reduced && !reduced.value().items.empty() ? &model::itemData(reduced.value().items.front()) : nullptr;

        result.expectTrue(replacementWire && replacementWire.value().front().data.value("content", "") == "before after" &&
                              !replacementWire.value().front().data.contains("contentDelta") &&
                              !replacementWire.value().front().data.contains("baseContentBytes") && legacyWire &&
                              legacyWire.value().data.value("content", "") == "before after" &&
                              !legacyWire.value().data.contains("contentDelta"),
                          "default expanded and legacy item-content encodings remain exact full replacements");
        result.expectTrue(appendWire && appendWire.value().front().data.contains("content") &&
                              appendWire.value().front().data.at("content") == "" &&
                              appendWire.value().front().data.value("contentDelta", "") == " after" &&
                              appendWire.value().front().data.value("baseContentBytes", std::uint64_t{0}) == 6 && decodedUpdate != nullptr &&
                              decodedUpdate->appendWireRepresentation && !decodedUpdate->content.has_value() &&
                              decodedUpdate->appendHint == std::optional<model::ItemContentAppendHint>{{6, " after"}} && reducedItem != nullptr &&
                              reducedItem->agentText == std::optional<std::string>{"before after"} && !reducedItem->contentTruncated &&
                              reducedItem->droppedContentBytes == std::optional<std::uint64_t>{0},
                          "append-v1 encodes only the suffix and applies it to the exact retained byte base");

        model::ItemContentUpdatedOccurrence staleHint{model::ItemIdentity{"item-optional-content"}};
        staleHint.threadId = model::ThreadIdentity{"thread-optional-content"};
        staleHint.turnId = model::TurnIdentity{"turn-optional-content"};
        staleHint.channel = "agentText";
        staleHint.content = "before after later";
        staleHint.appendHint = model::ItemContentAppendHint{6, " after"};
        const auto staleOccurrence = model::makeOccurrence(occurrenceIdentity(12, "stale-append-content"), std::move(staleHint));
        const auto staleWire =
            staleOccurrence
                ? model::encodeExpandedOccurrence(staleOccurrence.value(), model::ItemContentWireMode::AppendV1)
                : model::OccurrenceResult<std::vector<frontend::ExpandedFrontendEvent>>{staleOccurrence.error()};
        result.expectTrue(staleWire && staleWire.value().front().data.value("content", "") == "before after later" &&
                              !staleWire.value().front().data.contains("contentDelta") &&
                              !staleWire.value().front().data.contains("baseContentBytes"),
                          "append-v1 falls back to the authoritative replacement when a hint does not span its exact suffix");

        frontend::ExpandedFrontendEvent nonemptyHybrid = appendWire.value().front();
        nonemptyHybrid.data["content"] = "ambiguous";
        frontend::ExpandedFrontendEvent missingContent = appendWire.value().front();
        missingContent.data.erase("content");
        frontend::ExpandedFrontendEvent incompleteAppend = appendWire.value().front();
        incompleteAppend.data.erase("contentDelta");
        frontend::ExpandedFrontendEvent invalidDelta = appendWire.value().front();
        invalidDelta.data["contentDelta"] = frontend::Json::object();
        frontend::ExpandedFrontendEvent invalidBase = appendWire.value().front();
        invalidBase.data["baseContentBytes"] = -1;
        frontend::ExpandedFrontendEvent emptyReplacement = appendWire.value().front();
        emptyReplacement.data.erase("contentDelta");
        emptyReplacement.data.erase("baseContentBytes");
        const auto decodedEmptyReplacement =
            model::decodeExpandedOccurrence(emptyReplacement, context(), model::ItemContentWireMode::AppendV1);
        const auto* emptyReplacementUpdate =
            decodedEmptyReplacement && !decodedEmptyReplacement.value().expandedPayloads().empty()
                ? std::get_if<model::ItemContentUpdatedOccurrence>(
                      &decodedEmptyReplacement.value().expandedPayloads().front())
                : nullptr;
        result.expectTrue(!model::decodeExpandedOccurrence(nonemptyHybrid, context(), model::ItemContentWireMode::AppendV1) &&
                              !model::decodeExpandedOccurrence(missingContent, context(), model::ItemContentWireMode::AppendV1) &&
                              !model::decodeExpandedOccurrence(incompleteAppend, context(), model::ItemContentWireMode::AppendV1) &&
                              !model::decodeExpandedOccurrence(invalidDelta, context(), model::ItemContentWireMode::AppendV1) &&
                              !model::decodeExpandedOccurrence(invalidBase, context(), model::ItemContentWireMode::AppendV1) &&
                              emptyReplacementUpdate != nullptr &&
                              emptyReplacementUpdate->content == std::optional<std::string>{""} &&
                              !emptyReplacementUpdate->appendWireRepresentation,
                          "item-content wire data rejects absent, incomplete, ill-typed, and nonempty hybrid append forms while an "
                          "ordinary empty content-only replacement remains valid");

        frontend::ExpandedFrontendEvent mismatch = appendWire.value().front();
        mismatch.sequence = frontend::SequenceNumber{12};
        mismatch.data["baseContentBytes"] = std::uint64_t{5};
        const auto mismatchOccurrence =
            model::decodeExpandedOccurrence(mismatch, context(), model::ItemContentWireMode::AppendV1);
        model::CanonicalSnapshot unchanged = snapshotWithItem();
        const auto mismatchApplied = mismatchOccurrence ? model::applyOccurrence(unchanged, mismatchOccurrence.value())
                                                        : model::ModelResult<bool>{{model::ModelErrorCode::InvalidShape,
                                                                                   "/event",
                                                                                   "decode failed"}};
        result.expectTrue(mismatchOccurrence && !mismatchApplied &&
                              model::itemData(unchanged.items.front()).agentText == std::optional<std::string>{"before"},
                          "an append-v1 base mismatch is rejected atomically without changing canonical content");

        model::CanonicalSnapshot bounded = snapshotWithItem();
        std::visit(
            [](auto& item) {
                item.value.agentText = std::string(16'380, 'x');
                item.value.contentTruncated = false;
                item.value.droppedContentBytes = 0;
            },
            bounded.items.front());
        frontend::ExpandedFrontendEvent boundedAppend{
            frontend::SequenceNumber{13},
            frontend::ExpandedEventType::ItemContentUpdated,
            {{"threadId", "thread-optional-content"},
             {"turnId", "turn-optional-content"},
             {"itemId", "item-optional-content"},
             {"channel", "agentText"},
             {"content", ""},
             {"contentDelta", "abcdefgh"},
             {"baseContentBytes", std::uint64_t{16'380}},
             {"contentTruncated", true},
             {"droppedContentBytes", std::uint64_t{4}}}};
        const auto boundedOccurrence =
            model::decodeExpandedOccurrence(boundedAppend, context(), model::ItemContentWireMode::AppendV1);
        const auto boundedReduced = boundedOccurrence
                                        ? model::reduceOccurrence(bounded, boundedOccurrence.value())
                                        : model::ModelResult<model::CanonicalSnapshot>{
                                              {model::ModelErrorCode::InvalidShape, "/event", "decode failed"}};
        const model::ItemData* boundedItem = boundedReduced && !boundedReduced.value().items.empty()
                                                 ? &model::itemData(boundedReduced.value().items.front())
                                                 : nullptr;
        result.expectTrue(boundedItem != nullptr && boundedItem->agentText.has_value() && boundedItem->agentText->size() == 16'388 &&
                              boundedItem->agentText->ends_with("abcdefgh") && boundedItem->contentTruncated &&
                              boundedItem->droppedContentBytes == std::optional<std::uint64_t>{4},
                          "append-v1 extends negotiated agent text while preserving authoritative backend truncation metadata");

        const std::string overflowPrefix(16'384, 'p');
        const std::string overflowSuffix = std::string{" retained "} + "\xE2\x82\xAC";
        model::ItemContentUpdatedOccurrence overflow{model::ItemIdentity{"item-optional-content"}};
        overflow.threadId = model::ThreadIdentity{"thread-optional-content"};
        overflow.turnId = model::TurnIdentity{"turn-optional-content"};
        overflow.channel = "agentText";
        overflow.content = overflowPrefix;
        overflow.truncation.truncated = true;
        overflow.truncation.droppedBytes = static_cast<std::uint64_t>(overflowSuffix.size());
        overflow.overflowV1 = model::ItemContentOverflowV1{static_cast<std::uint64_t>(overflowPrefix.size()),
                                                          overflowSuffix,
                                                          0,
                                                          false,
                                                          false};
        const auto overflowOccurrence =
            model::makeOccurrence(occurrenceIdentity(14, "overflow-content"), std::move(overflow));
        const auto overflowReplacementWire =
            overflowOccurrence
                ? model::encodeExpandedOccurrence(overflowOccurrence.value())
                : model::OccurrenceResult<std::vector<frontend::ExpandedFrontendEvent>>{overflowOccurrence.error()};
        const auto overflowWire =
            overflowOccurrence
                ? model::encodeExpandedOccurrence(overflowOccurrence.value(), model::ItemContentWireMode::AppendV1)
                : model::OccurrenceResult<std::vector<frontend::ExpandedFrontendEvent>>{overflowOccurrence.error()};
        if (!overflowWire) {
            result.expectTrue(false, "item-content overflow occurrence encodes for append-v1");
            return;
        }
        const auto overflowDecoded =
            overflowWire ? model::decodeExpandedOccurrence(overflowWire.value().front(), context(), model::ItemContentWireMode::AppendV1)
                         : model::OccurrenceResult<model::CanonicalOccurrence>{overflowWire.error()};
        const auto overflowReduced =
            overflowDecoded
                ? model::reduceOccurrence(snapshotWithItem(), overflowDecoded.value())
                : model::ModelResult<model::CanonicalSnapshot>{
                      {model::ModelErrorCode::InvalidShape, "/event", "decode failed"}};
        const model::ItemData* overflowItem =
            overflowReduced && !overflowReduced.value().items.empty()
                ? &model::itemData(overflowReduced.value().items.front())
                : nullptr;
        frontend::ExpandedFrontendEvent malformedOverflow = overflowWire.value().front();
        malformedOverflow.data[std::string(model::ItemContentOverflowV1Property)][0] = std::uint64_t{16'383};
        result.expectTrue(
            overflowReplacementWire && overflowWire &&
                !overflowReplacementWire.value().front().data.contains(std::string(model::ItemContentOverflowV1Property)) &&
                overflowReplacementWire.value().front().data.value("content", "") == overflowPrefix &&
                overflowWire.value().front().data.contains(std::string(model::ItemContentOverflowV1Property)) && overflowItem != nullptr &&
                overflowItem->agentText == std::optional<std::string>{overflowPrefix + overflowSuffix} &&
                !overflowItem->contentTruncated && overflowItem->droppedContentBytes == std::optional<std::uint64_t>{0} &&
                !model::decodeExpandedOccurrence(
                    overflowWire.value().front(), context(), model::ItemContentWireMode::Replacement) &&
                !model::decodeExpandedOccurrence(malformedOverflow, context(), model::ItemContentWireMode::AppendV1),
            "only append-v1 restores a UTF-8 overflow suffix while replacement peers keep the truthful prefix and malformed bases fail");

        model::ItemContentUpdatedOccurrence pastFrozen{model::ItemIdentity{"item-optional-content"}};
        pastFrozen.threadId = model::ThreadIdentity{"thread-optional-content"};
        pastFrozen.turnId = model::TurnIdentity{"turn-optional-content"};
        pastFrozen.channel = "agentText";
        pastFrozen.content = overflowPrefix;
        pastFrozen.truncation.truncated = true;
        pastFrozen.truncation.droppedBytes = static_cast<std::uint64_t>(overflowSuffix.size() + 5);
        pastFrozen.overflowV1 = model::ItemContentOverflowV1{static_cast<std::uint64_t>(overflowPrefix.size()),
                                                            overflowSuffix + " next",
                                                            0,
                                                            false,
                                                            false};
        pastFrozen.appendHint = model::ItemContentAppendHint{
            static_cast<std::uint64_t>(overflowPrefix.size() + overflowSuffix.size()), " next", 0, true};
        const auto pastFrozenOccurrence =
            model::makeOccurrence(occurrenceIdentity(15, "past-frozen-content"), std::move(pastFrozen));
        const auto pastFrozenWire =
            pastFrozenOccurrence
                ? model::encodeExpandedOccurrence(pastFrozenOccurrence.value(), model::ItemContentWireMode::AppendV1)
                : model::OccurrenceResult<std::vector<frontend::ExpandedFrontendEvent>>{pastFrozenOccurrence.error()};
        const auto pastFrozenDecoded =
            pastFrozenWire
                ? model::decodeExpandedOccurrence(pastFrozenWire.value().front(), context(), model::ItemContentWireMode::AppendV1)
                : model::OccurrenceResult<model::CanonicalOccurrence>{pastFrozenWire.error()};
        const auto pastFrozenReduced =
            pastFrozenDecoded && overflowReduced
                ? model::reduceOccurrence(overflowReduced.value(), pastFrozenDecoded.value())
                : model::ModelResult<model::CanonicalSnapshot>{
                      {model::ModelErrorCode::InvalidShape, "/event", "decode failed"}};
        const model::ItemData* pastFrozenItem =
            pastFrozenReduced && !pastFrozenReduced.value().items.empty()
                ? &model::itemData(pastFrozenReduced.value().items.front())
                : nullptr;
        result.expectTrue(pastFrozenWire && pastFrozenWire.value().front().data.value("contentDelta", "") == " next" &&
                              pastFrozenWire.value().front().data.value("baseContentBytes", std::uint64_t{0}) ==
                                  overflowPrefix.size() + overflowSuffix.size() &&
                              pastFrozenItem != nullptr &&
                              pastFrozenItem->agentText == std::optional<std::string>{overflowPrefix + overflowSuffix + " next"},
                          "append-v1 continues with exact deltas after the frozen agentText field boundary");

        model::CanonicalSnapshot snapshotOverflow = snapshotWithItem();
        std::visit(
            [&](auto& item) {
                item.value.agentText = overflowPrefix;
                item.value.agentTextOverflowV1 = model::ItemContentOverflowV1{
                    static_cast<std::uint64_t>(overflowPrefix.size()), overflowSuffix, 0, false, false};
                item.value.contentTruncated = true;
                item.value.droppedContentBytes = static_cast<std::uint64_t>(overflowSuffix.size());
                item.value.truncation.truncated = true;
                item.value.truncation.droppedBytes = static_cast<std::uint64_t>(overflowSuffix.size());
                item.value.truncation.omittedPaths = {"/agentText"};
            },
            snapshotOverflow.items.front());
        const auto encodedOverflowSnapshot = model::encodeSnapshot(snapshotOverflow, model::ItemContentWireMode::AppendV1);
        const auto decodedOverflowSnapshot =
            encodedOverflowSnapshot
                ? model::decodeSnapshot(encodedOverflowSnapshot.value(), model::ItemContentWireMode::AppendV1)
                : model::ModelResult<model::CanonicalSnapshot>{encodedOverflowSnapshot.error()};
        const model::ItemData* snapshotOverflowItem =
            decodedOverflowSnapshot && !decodedOverflowSnapshot.value().items.empty()
                ? &model::itemData(decodedOverflowSnapshot.value().items.front())
                : nullptr;
        result.expectTrue(snapshotOverflowItem != nullptr &&
                              snapshotOverflowItem->agentText == std::optional<std::string>{overflowPrefix + overflowSuffix} &&
                              !snapshotOverflowItem->contentTruncated &&
                              snapshotOverflowItem->droppedContentBytes == std::optional<std::uint64_t>{0},
                          "append-v1 snapshot decoding restores the same bounded suffix used by live and terminal item replacements");

        const std::string commandSuffix = std::string(20'000, 'o') + "\xE2\x82\xAC";
        model::ItemContentUpdatedOccurrence commandOverflow{model::ItemIdentity{"command-overflow-item"}};
        commandOverflow.threadId = model::ThreadIdentity{"command-overflow-thread"};
        commandOverflow.turnId = model::TurnIdentity{"command-overflow-turn"};
        commandOverflow.channel = "commandOutput";
        commandOverflow.itemKind = frontend::ThreadItemKind::CommandExecution;
        commandOverflow.content = overflowPrefix;
        commandOverflow.truncation.truncated = true;
        commandOverflow.truncation.droppedBytes = static_cast<std::uint64_t>(commandSuffix.size());
        commandOverflow.overflowV1 = model::ItemContentOverflowV1{static_cast<std::uint64_t>(overflowPrefix.size()),
                                                                 commandSuffix,
                                                                 0,
                                                                 false,
                                                                 false};
        const auto commandOccurrence =
            model::makeOccurrence(occurrenceIdentity(16, "command-overflow-content"), std::move(commandOverflow));
        const auto commandV1Wire =
            commandOccurrence
                ? model::encodeExpandedOccurrence(commandOccurrence.value(), model::ItemContentWireMode::AppendV1)
                : model::OccurrenceResult<std::vector<frontend::ExpandedFrontendEvent>>{commandOccurrence.error()};
        const auto commandV2Wire =
            commandOccurrence
                ? model::encodeExpandedOccurrence(commandOccurrence.value(), model::ItemContentWireMode::AppendV2)
                : model::OccurrenceResult<std::vector<frontend::ExpandedFrontendEvent>>{commandOccurrence.error()};
        const auto commandDecoded =
            commandV2Wire
                ? model::decodeExpandedOccurrence(commandV2Wire.value().front(), context(), model::ItemContentWireMode::AppendV2)
                : model::OccurrenceResult<model::CanonicalOccurrence>{commandV2Wire.error()};
        model::CanonicalSnapshot commandInitial;
        model::ItemData commandInitialData{model::ItemIdentity{"command-overflow-item"},
                                           model::ThreadIdentity{"command-overflow-thread"},
                                           model::TurnIdentity{"command-overflow-turn"}};
        commandInitialData.commandOutput = "old";
        commandInitial.items.emplace_back(model::CommandExecutionItem{std::move(commandInitialData)});
        const auto commandReduced = commandDecoded
                                        ? model::reduceOccurrence(commandInitial, commandDecoded.value())
                                        : model::ModelResult<model::CanonicalSnapshot>{
                                              {model::ModelErrorCode::InvalidShape, "/event", "decode failed"}};
        const model::ItemData* commandReducedItem = commandReduced && !commandReduced.value().items.empty()
                                                        ? &model::itemData(commandReduced.value().items.front())
                                                        : nullptr;

        model::CanonicalSnapshot commandSnapshot;
        model::ItemData commandSnapshotData{model::ItemIdentity{"command-overflow-item"},
                                            model::ThreadIdentity{"command-overflow-thread"},
                                            model::TurnIdentity{"command-overflow-turn"}};
        commandSnapshotData.commandOutput = overflowPrefix;
        commandSnapshotData.commandOutputOverflowV2 = model::ItemContentOverflowV1{
            static_cast<std::uint64_t>(overflowPrefix.size()), commandSuffix, 0, false, false};
        commandSnapshotData.contentTruncated = true;
        commandSnapshotData.droppedContentBytes = static_cast<std::uint64_t>(commandSuffix.size());
        commandSnapshotData.truncation.truncated = true;
        commandSnapshotData.truncation.droppedBytes = static_cast<std::uint64_t>(commandSuffix.size());
        commandSnapshotData.truncation.omittedPaths = {"/commandOutput"};
        commandSnapshot.items.emplace_back(model::CommandExecutionItem{std::move(commandSnapshotData)});
        const auto encodedCommandV1Snapshot = model::encodeSnapshot(commandSnapshot, model::ItemContentWireMode::AppendV1);
        const auto encodedCommandV2Snapshot = model::encodeSnapshot(commandSnapshot, model::ItemContentWireMode::AppendV2);
        const auto decodedCommandV2Snapshot =
            encodedCommandV2Snapshot
                ? model::decodeSnapshot(encodedCommandV2Snapshot.value(), model::ItemContentWireMode::AppendV2)
                : model::ModelResult<model::CanonicalSnapshot>{encodedCommandV2Snapshot.error()};
        const model::ItemData* commandSnapshotItem =
            decodedCommandV2Snapshot && !decodedCommandV2Snapshot.value().items.empty()
                ? &model::itemData(decodedCommandV2Snapshot.value().items.front())
                : nullptr;
        const frontend::Json* commandOverflowWire =
            commandV2Wire && commandV2Wire.value().front().data.contains(std::string(model::CommandOutputOverflowV2Property))
                ? &commandV2Wire.value().front().data.at(std::string(model::CommandOutputOverflowV2Property))
                : nullptr;
        result.expectTrue(commandV1Wire &&
                              !commandV1Wire.value().front().data.contains(
                                  std::string(model::CommandOutputOverflowV2Property)) &&
                              commandV2Wire && commandOverflowWire != nullptr && commandOverflowWire->is_object() &&
                              commandOverflowWire->size() == 5 && commandOverflowWire->at("chunksBase64").is_array() &&
                              commandOverflowWire->at("chunksBase64").size() >= 2,
                          "append-v2 emits bounded multi-chunk command output while append-v1 retains the frozen prefix");
        result.expectTrue(commandDecoded && commandReducedItem != nullptr &&
                              commandReducedItem->commandOutput == std::optional<std::string>{overflowPrefix + commandSuffix} &&
                              !commandReducedItem->contentTruncated &&
                              commandReducedItem->droppedContentBytes == std::optional<std::uint64_t>{0} &&
                              !model::decodeExpandedOccurrence(
                                  commandV2Wire.value().front(), context(), model::ItemContentWireMode::AppendV1),
                          "append-v2 command overflow decodes and applies at the retained 4 MiB channel bound only when negotiated");
        result.expectTrue(encodedCommandV1Snapshot && encodedCommandV2Snapshot && decodedCommandV2Snapshot &&
                              commandSnapshotItem != nullptr &&
                              commandSnapshotItem->commandOutput == std::optional<std::string>{overflowPrefix + commandSuffix} &&
                              !commandSnapshotItem->contentTruncated &&
                              commandSnapshotItem->droppedContentBytes == std::optional<std::uint64_t>{0},
                          "append-v2 snapshots restore complete backend-bounded command output");

        model::CanonicalSnapshot dualOverflowSnapshot;
        model::ItemData dualData{model::ItemIdentity{"dual-overflow-item"},
                                 model::ThreadIdentity{"dual-overflow-thread"},
                                 model::TurnIdentity{"dual-overflow-turn"}};
        const std::string dualAgentSuffix = " agent tail";
        const std::string dualCommandSuffix = " command tail";
        constexpr std::uint64_t dualBaselineDropped = 7;
        dualData.agentText = overflowPrefix;
        dualData.commandOutput = overflowPrefix;
        dualData.agentTextOverflowV1 = model::ItemContentOverflowV1{
            static_cast<std::uint64_t>(overflowPrefix.size()), dualAgentSuffix, dualBaselineDropped, true, true};
        dualData.commandOutputOverflowV2 = model::ItemContentOverflowV1{
            static_cast<std::uint64_t>(overflowPrefix.size()), dualCommandSuffix, dualBaselineDropped, true, true};
        dualData.droppedContentBytes = dualBaselineDropped + dualAgentSuffix.size() + dualCommandSuffix.size();
        dualData.contentTruncated = true;
        dualData.truncation.truncated = true;
        dualData.truncation.droppedBytes = *dualData.droppedContentBytes;
        dualData.truncation.omittedPaths = {"/agentText", "/commandOutput"};
        dualOverflowSnapshot.items.emplace_back(model::CommandExecutionItem{std::move(dualData)});
        const auto dualEncoded = model::encodeSnapshot(dualOverflowSnapshot, model::ItemContentWireMode::AppendV2);
        const auto dualDecoded = dualEncoded
                                     ? model::decodeSnapshot(dualEncoded.value(), model::ItemContentWireMode::AppendV2)
                                     : model::ModelResult<model::CanonicalSnapshot>{dualEncoded.error()};
        const model::ItemData* dualItem = dualDecoded && dualDecoded.value().items.size() == 1
                                              ? &model::itemData(dualDecoded.value().items.front())
                                              : nullptr;
        result.expectTrue(dualItem &&
                              dualItem->agentText == std::optional<std::string>{overflowPrefix + dualAgentSuffix} &&
                              dualItem->commandOutput == std::optional<std::string>{overflowPrefix + dualCommandSuffix} &&
                              dualItem->droppedContentBytes == std::optional<std::uint64_t>{dualBaselineDropped} &&
                              dualItem->contentTruncated && dualItem->truncation.truncated,
                          "append-v2 snapshots restore agent and command overflow atomically against one aggregate baseline");

        model::CanonicalSnapshot escapingSnapshot;
        model::ItemData escapingData{model::ItemIdentity{"escaping-overflow-item"},
                                     model::ThreadIdentity{"escaping-overflow-thread"},
                                     model::TurnIdentity{"escaping-overflow-turn"}};
        const std::string escapingPrefix = "p";
        const std::string escapingSuffix(model::MaximumCommandOutputOverflowV2Bytes - escapingPrefix.size(), '"');
        escapingData.commandOutput = escapingPrefix;
        escapingData.commandOutputOverflowV2 = model::ItemContentOverflowV1{
            static_cast<std::uint64_t>(escapingPrefix.size()), escapingSuffix, 0, false, false};
        escapingData.droppedContentBytes = escapingSuffix.size();
        escapingData.contentTruncated = true;
        escapingData.truncation.truncated = true;
        escapingData.truncation.droppedBytes = escapingSuffix.size();
        escapingData.truncation.omittedPaths = {"/commandOutput"};
        escapingSnapshot.items.emplace_back(model::CommandExecutionItem{std::move(escapingData)});
        const auto escapingEncoded = model::encodeSnapshot(escapingSnapshot, model::ItemContentWireMode::AppendV2);
        const auto escapingDecoded = escapingEncoded
                                         ? model::decodeSnapshot(escapingEncoded.value(), model::ItemContentWireMode::AppendV2)
                                         : model::ModelResult<model::CanonicalSnapshot>{escapingEncoded.error()};
        const model::ItemData* escapingItem = escapingDecoded && escapingDecoded.value().items.size() == 1
                                                  ? &model::itemData(escapingDecoded.value().items.front())
                                                  : nullptr;
        const frontend::Json* escapingWireData =
            escapingEncoded && escapingEncoded.value().state.items &&
                    escapingEncoded.value().state.items->size() == 1 &&
                    escapingEncoded.value().state.items->front().data
                ? &*escapingEncoded.value().state.items->front().data
                : nullptr;
        result.expectTrue(escapingEncoded &&
                              escapingWireData &&
                              escapingWireData->contains(std::string(model::CommandOutputOverflowV2Property)) &&
                              escapingWireData->at(std::string(model::CommandOutputOverflowV2Property)).dump().size() <=
                                  model::MaximumCommandOutputOverflowV2EncodedBytes &&
                              escapingItem &&
                              escapingItem->commandOutput == std::optional<std::string>{escapingPrefix + escapingSuffix} &&
                              !escapingItem->contentTruncated &&
                              escapingItem->droppedContentBytes == std::optional<std::uint64_t>{0},
                          "JSON-expansion-heavy command output uses bounded base64 chunks and restores completely");

        const auto commandOverflowWireValue = [](frontend::Json chunks, std::uint64_t baseContentBytes = 0) {
            return frontend::Json{{"baseContentBytes", baseContentBytes},
                                  {"chunksBase64", std::move(chunks)},
                                  {"droppedContentBytesBeforeProjection", 0},
                                  {"contentTruncatedBeforeProjection", false},
                                  {"truncationBeforeProjection", false}};
        };
        const auto nonCanonicalBase64 = model::decodeCommandOutputOverflowV2(
            commandOverflowWireValue(frontend::Json::array({"AB=="})), "/commandOverflow");
        const auto oversizedBase64Chunk = model::decodeCommandOutputOverflowV2(
            commandOverflowWireValue(frontend::Json::array({std::string(16U * 1024U + 4U, 'A')})),
            "/commandOverflow");
        frontend::Json tooManyBase64Chunks = frontend::Json::array();
        for (std::size_t index = 0; index < 343; ++index) {
            tooManyBase64Chunks.push_back("AAAA");
        }
        const auto excessiveBase64ChunkCount = model::decodeCommandOutputOverflowV2(
            commandOverflowWireValue(std::move(tooManyBase64Chunks)), "/commandOverflow");
        const auto decodedCommandOutputOverCapacity = model::decodeCommandOutputOverflowV2(
            commandOverflowWireValue(frontend::Json::array({"AAAA"}), model::MaximumCommandOutputOverflowV2Bytes),
            "/commandOverflow");
        result.expectTrue(!nonCanonicalBase64 && !oversizedBase64Chunk && !excessiveBase64ChunkCount &&
                              !decodedCommandOutputOverCapacity,
                          "append-v2 rejects non-canonical base64, over-capacity chunks, excessive chunk counts, and decoded output "
                          "beyond the retained command-output bound");

        const auto snapshotWithDetails = [&](bool reservedCollision) {
            model::CanonicalSnapshot value = snapshotOverflow;
            frontend::Json details = frontend::Json::object();
            const std::size_t ordinaryMembers = reservedCollision ? 63 : 64;
            for (std::size_t index = 0; index < ordinaryMembers; ++index) {
                details["detail" + std::to_string(index)] = static_cast<std::uint64_t>(index);
            }
            if (reservedCollision) {
                details[std::string(model::ItemContentOverflowV1Property)] = "provider collision";
            }
            std::visit(
                [&](auto& item) {
                    item.value.safeDetails = *model::SafeDetail::fromJson(std::move(details));
                },
                value.items.front());
            return value;
        };
        const auto capacitySnapshot =
            model::encodeSnapshot(snapshotWithDetails(false), model::ItemContentWireMode::AppendV1);
        const auto collisionSnapshot =
            model::encodeSnapshot(snapshotWithDetails(true), model::ItemContentWireMode::AppendV1);
        const frontend::ExpandedThreadItem* capacityItem =
            capacitySnapshot && capacitySnapshot.value().state.items.has_value() &&
                    !capacitySnapshot.value().state.items->empty()
                ? &capacitySnapshot.value().state.items->front()
                : nullptr;
        const frontend::ExpandedThreadItem* collisionItem =
            collisionSnapshot && collisionSnapshot.value().state.items.has_value() &&
                    !collisionSnapshot.value().state.items->empty()
                ? &collisionSnapshot.value().state.items->front()
                : nullptr;
        const bool capacityAccounted =
            capacityItem != nullptr && capacityItem->data.has_value() && capacityItem->data->size() == 64 && capacityItem->truncated &&
            std::find(capacityItem->omittedFields.begin(), capacityItem->omittedFields.end(), "/data") !=
                capacityItem->omittedFields.end() &&
            capacityItem->data->contains(std::string(model::ItemContentOverflowV1Property));
        const bool collisionAccounted =
            collisionItem != nullptr && collisionItem->data.has_value() && collisionItem->truncated &&
            std::find(collisionItem->omittedFields.begin(), collisionItem->omittedFields.end(), "/data") !=
                collisionItem->omittedFields.end() &&
            collisionItem->data->at(std::string(model::ItemContentOverflowV1Property)).is_array();
        result.expectTrue(capacityAccounted && collisionAccounted &&
                              !model::decodeSnapshot(
                                  collisionSnapshot.value(), model::ItemContentWireMode::Replacement),
                          "overflow reserves one bounded detail member, accounts capacity/collisions truthfully, and is rejected without "
                          "append-v1 negotiation");
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

    void testSparseCollectionOccurrencePresence(tests::support::TestResult& result) {
        model::ProcessState process{model::ProcessHandle{"process-sparse"}};
        model::FilesystemWatchRecord watch;
        watch.watchId = "watch-sparse";
        model::FuzzySearchRecord search;
        search.sessionId = "search-sparse";
        model::NoticeRecord notice;
        notice.occurrence = 28;
        notice.category = "information";
        notice.summary = "sparse notice";
        model::ActivityRecord activity;
        activity.key = "activity-sparse";
        activity.kind = "tool";
        activity.lifecycle = "running";
        activity.active = true;

        const auto makeIdentity = [](std::uint64_t sequence, std::string group) {
            return model::OccurrenceIdentity{model::FrontendSequence{sequence},
                                             model::OccurrenceGroupIdentity{std::move(group)},
                                             0,
                                             1,
                                             model::SourceStamp{"backend:sparse-collection"}};
        };
        const auto processOccurrence = model::makeOccurrence(makeIdentity(27, "process-sparse-group"),
                                                             model::OccurrencePayload{model::ProcessUpdatedOccurrence{std::move(process)}});
        const auto watchOccurrence = model::makeOccurrence(
            makeIdentity(28, "watch-sparse-group"), model::OccurrencePayload{model::FilesystemWatchUpdatedOccurrence{std::move(watch)}});
        const auto searchOccurrence = model::makeOccurrence(
            makeIdentity(29, "search-sparse-group"), model::OccurrencePayload{model::FuzzySearchUpdatedOccurrence{std::move(search)}});
        const auto noticeOccurrence = model::makeOccurrence(makeIdentity(30, "notice-sparse-group"),
                                                            model::OccurrencePayload{model::NoticeAddedOccurrence{std::move(notice)}});
        const auto activityOccurrence = model::makeOccurrence(
            makeIdentity(31, "activity-sparse-group"), model::OccurrencePayload{model::ActivityUpdatedOccurrence{std::move(activity)}});
        model::CanonicalSnapshot absentProcessSnapshot;
        absentProcessSnapshot.projection.absentPaths.push_back("/processes");
        const auto processState =
            processOccurrence
                ? model::reduceOccurrence(absentProcessSnapshot, processOccurrence.value())
                : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        const auto watchState =
            watchOccurrence
                ? model::reduceOccurrence(model::CanonicalSnapshot{}, watchOccurrence.value())
                : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        const auto searchState =
            searchOccurrence
                ? model::reduceOccurrence(model::CanonicalSnapshot{}, searchOccurrence.value())
                : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        const auto noticeState =
            noticeOccurrence
                ? model::reduceOccurrence(model::CanonicalSnapshot{}, noticeOccurrence.value())
                : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};
        const auto activityState =
            activityOccurrence
                ? model::reduceOccurrence(model::CanonicalSnapshot{}, activityOccurrence.value())
                : model::ModelResult<model::CanonicalSnapshot>{{model::ModelErrorCode::InvalidShape, "/event", "creation failed"}};

        result.expectTrue(processState && processState.value().processes.size() == 1 &&
                              std::ranges::find(processState.value().projection.absentPaths, "/processes") ==
                                  processState.value().projection.absentPaths.end() &&
                              watchState && watchState.value().filesystemWatches.state.information == model::InformationState::Present &&
                              watchState.value().filesystemWatches.entries.size() == 1 && searchState &&
                              searchState.value().fuzzySearches.state.information == model::InformationState::Present &&
                              searchState.value().fuzzySearches.entries.size() == 1 && noticeState &&
                              noticeState.value().notices.state.information == model::InformationState::Present &&
                              noticeState.value().notices.entries.size() == 1 && activityState &&
                              activityState.value().activities.state.information == model::InformationState::Present &&
                              activityState.value().activities.entries.size() == 1,
                          "sparse record occurrences make an absent collection represented before retaining its first entry");
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
    testInPlaceAndCopyPreservingReduction(result);
    testItemContentPresence(result);
    testNegotiatedItemContentAppend(result);
    testItemOrderingAndAuthorityMigration(result);
    testScopedItemIdentityReduction(result);
    testForkTurnDescendantScoping(result);
    testUnicodeItemContentReduction(result);
    testPendingOrderingAndAuthorityMigration(result);
    testDescendantReplacementOrdering(result);
    testTurnPlanOccurrenceRoundTrip(result);
    testDiagnosticOccurrenceShape(result);
    testSparseCollectionOccurrencePresence(result);
    testLegacyNestedOccurrenceRoundTrips(result);
    testLegacyPendingPresentationAndDiagnostics(result);
    testOccurrenceIdentityValidation(result);
    testLegacyOnlyExpandedEncodingGuards(result);
    testServerCompatibilitySelection(result);
    return result.processResult();
}
