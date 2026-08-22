/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/backend/Snapshot.h"
#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/EventCoalescer.h"
#include "ai/openai/codex/frontend/UpdateBatch.h"
#include "ai/openai/codex/frontend/internal/model/Occurrence.h"
#include "ai/openai/codex/frontend/internal/server/BackendCoreBridge.h"
#include "support/TestResult.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace {
    namespace backend = ai::openai::codex::backend;
    namespace frontend = ai::openai::codex::frontend;
    namespace model = ai::openai::codex::frontend::internal::model;
    namespace server = ai::openai::codex::frontend::internal::server;
    namespace typed = ai::openai::codex::typed;

    void testCoalescingAndBounds(tests::support::TestResult& result) {
        frontend::EventCoalescer coalescer({32});
        const frontend::CoalescingKey agentKey = frontend::CoalescingKey::itemContent("thread-1", "turn-1", "item-1", "agentText");

        std::string accumulated;
        std::size_t schedulingRequests = 0;
        for (std::size_t index = 0; index < 1000; ++index) {
            accumulated += static_cast<char>('a' + static_cast<int>(index % 26));
            const frontend::CoalescerMarkResult marked = coalescer.mark({agentKey,
                                                                         "item.content.updated",
                                                                         frontend::Json{{"threadId", "thread-1"},
                                                                                        {"turnId", "turn-1"},
                                                                                        {"itemId", "item-1"},
                                                                                        {"channel", "agentText"},
                                                                                        {"content", accumulated}},
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
                                               frontend::Json{{"threadId", "thread-1"},
                                                              {"turnId", "turn-1"},
                                                              {"itemId", "item-1"},
                                                              {"channel", "reasoningText"},
                                                              {"content", "reasoning-final"}},
                                               frontend::FlushUrgency::Deferred});
        const auto commandOutput = coalescer.mark({frontend::CoalescingKey::itemContent("thread-1", "turn-1", "item-1", "commandOutput"),
                                                   "item.content.updated",
                                                   frontend::Json{{"threadId", "thread-1"},
                                                                  {"turnId", "turn-1"},
                                                                  {"itemId", "item-1"},
                                                                  {"channel", "commandOutput"},
                                                                  {"content", "command-final"}},
                                                   frontend::FlushUrgency::Deferred});
        const auto otherTurn = coalescer.mark({frontend::CoalescingKey::itemContent("thread-1", "turn-2", "item-1", "reasoningText"),
                                               "item.content.updated",
                                               frontend::Json{{"threadId", "thread-1"},
                                                              {"turnId", "turn-2"},
                                                              {"itemId", "item-1"},
                                                              {"channel", "reasoningText"},
                                                              {"content", "other-turn"}},
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
                                     frontend::Json{{"threadId", "thread-order"},
                                                    {"turnId", "turn-order"},
                                                    {"itemId", "item-order"},
                                                    {"channel", "agentText"},
                                                    {"content", "final"}},
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

    void testThreadReadSnapshotPreservesCanonicalMetadata(tests::support::TestResult& result) {
        const frontend::Json executionConfiguration{
            {"approvalPolicy", "on-request"},
            {"approvalsReviewer", "user"},
            {"collaborationMode", {{"mode", "plan"}, {"settings", {{"model", "gpt-test"}}}}},
            {"cwd", "/workspace"},
            {"model", "gpt-test"},
            {"modelProvider", "openai"},
            {"sandboxPolicy",
             {{"type", "workspaceWrite"},
              {"networkAccess", false},
              {"writableRoots", frontend::Json::array({"/workspace"})}}},
        };

        backend::ItemSnapshot item;
        item.id = "item-metadata";
        item.type = "agentMessage";
        item.status = "completed";
        item.agentText = "retained content";
        item.stamp = {9, backend::Freshness::Stale};
        item.connectionInvalidated = true;

        backend::ItemSnapshot futureItem;
        futureItem.id = "item-future";
        futureItem.type = "future_super_item";
        futureItem.status = "completed";
        futureItem.data = frontend::Json{{"opaqueFutureValue", 42}};

        backend::ItemSnapshot userMessage;
        userMessage.id = "item-user-message";
        userMessage.type = "user_message";
        userMessage.status = "completed";
        userMessage.userMessage = backend::UserMessageSnapshot{"client-message",
                                                                "first\n\nsecond",
                                                                false,
                                                                false,
                                                                64,
                                                                64,
                                                                2,
                                                                2,
                                                                {"first", "second"}};

        const std::string longUserText(model::SafeDetail::HardMaximumBytes + 4U * 1024U, 'x');
        backend::ItemSnapshot longUserMessage;
        longUserMessage.id = "item-long-user-message";
        longUserMessage.type = "user_message";
        longUserMessage.status = "completed";
        longUserMessage.userMessage = backend::UserMessageSnapshot{
            "long-client-message", longUserText, false, false, longUserText.size(), longUserText.size(), 1, 1, {longUserText}};

        backend::ItemSnapshot escapedCommand;
        escapedCommand.id = "item-escaped-command";
        escapedCommand.type = "command_execution";
        escapedCommand.status = "completed";
        escapedCommand.commandOutput = std::string(20'000, '\\');

        backend::ItemSnapshot metadataOnly;
        metadataOnly.id = "item-metadata-only";
        metadataOnly.type = "hookPrompt";
        metadataOnly.status = "completed";
        metadataOnly.data = frontend::Json{{"providerOnly", "must-not-reach-the-wire"}};

        backend::TurnSnapshot turn;
        turn.id = "turn-metadata";
        turn.threadId = "thread-metadata";
        turn.status = "completed";
        turn.terminal = true;
        turn.plan = backend::TurnPlanState{"ordered work",
                                          {{"Inspect", typed::TurnPlanStepStatus::completed()}},
                                          1,
                                          false};
        turn.effectiveExecutionConfiguration = executionConfiguration;
        turn.effectiveExecutionConfigurationProvenance = "thread_settings_updated";
        turn.items.push_back(std::move(item));
        turn.items.push_back(std::move(futureItem));
        turn.items.push_back(std::move(userMessage));
        turn.items.push_back(std::move(longUserMessage));
        turn.items.push_back(std::move(escapedCommand));
        turn.items.push_back(std::move(metadataOnly));
        turn.stamp = {8, backend::Freshness::Current};
        turn.connectionInvalidated = true;

        backend::ThreadSnapshot thread;
        thread.id = "thread-metadata";
        thread.ephemeral = false;
        thread.archived = true;
        thread.executionConfiguration = executionConfiguration;
        thread.fullyLoaded = true;
        thread.turns.push_back(std::move(turn));
        thread.stamp = {7, backend::Freshness::Current};
        thread.realtime.lifecycle = "connected";
        thread.realtime.transcript = "hello";
        thread.realtime.sessionId = "realtime-session";
        thread.realtime.version = "v1";
        thread.realtime.itemCount = 6;
        thread.realtime.receivedAudioBytes = 5;
        thread.realtime.droppedAudioBytes = 2;
        thread.realtime.transcriptTruncated = true;
        thread.realtime.stamp = {6, backend::Freshness::Stale};

        const frontend::Json wire = server::BackendCoreBridgeTestAccess::boundedThreadReadResult(
            typed::ThreadId{"thread-metadata"}, thread, 1024U * 1024U);
        const auto validation = frontend::Codec::validateDefinedResult(frontend::generated::MethodId::ThreadRead, wire);
        const frontend::Json& encodedThread = wire.at("thread");
        const frontend::Json& encodedTurn = encodedThread.at("turns").at(0);
        const frontend::Json& encodedItem = encodedTurn.at("items").at(0);
        const frontend::Json& encodedFutureItem = encodedTurn.at("items").at(1);
        const frontend::Json& encodedUserMessage = encodedTurn.at("items").at(2);
        const frontend::Json& encodedMetadataOnly = encodedTurn.at("items").at(5);

        result.expectTrue(
            validation && wire.at("stateEffect").at("authority") == "replace" && encodedThread.at("ephemeral") == false &&
                encodedThread.at("archived") == true && encodedThread.at("executionConfiguration") == executionConfiguration &&
                encodedThread.at("stamp") == frontend::Json{{"generation", 7}, {"freshness", "current"}} &&
                encodedThread.at("realtime").at("sessionId") == "realtime-session" &&
                encodedThread.at("realtime").at("sourceGeneration") == 6 && encodedThread.at("realtime").at("sourceFreshness") == "stale" &&
                encodedTurn.at("plan").at("steps").at(0) == "Inspect" && encodedTurn.at("plan").at("statuses").at(0) == "completed" &&
                encodedTurn.at("effectiveExecutionConfiguration") == executionConfiguration &&
                encodedTurn.at("effectiveExecutionConfigurationProvenance") == "thread_settings_updated" &&
                encodedTurn.at("stamp") == frontend::Json{{"generation", 8}, {"freshness", "current"}} &&
                encodedTurn.at("connectionInvalidated") == true && encodedItem.at("generation") == 9 &&
                encodedItem.at("freshness") == "stale" && encodedItem.at("connectionInvalidated") == true &&
                encodedFutureItem.at("type") == "future_super_item" && encodedUserMessage.at("data").at("content").size() == 2 &&
                encodedUserMessage.at("data").at("content").at(0).at("text") == "first" &&
                encodedMetadataOnly.at("data") == frontend::Json{{"codexType", "hookPrompt"}} &&
                backend::threadSnapshotSizeBytes(thread) == encodedThread.dump().size(),
            validation ? "authoritative thread.read replacement preserves all canonical thread, turn, and item metadata"
                       : "state-complete authoritative thread.read result failed validation: " + validation.error().message);

        frontend::Json nestedThread = encodedThread;
        nestedThread["turns"][0]["items"][1]["extensions"] =
            frontend::Json{{"threadId", "thread-metadata"}, {"turnId", "turn-metadata"}, {"futureExtensionValue", 73}};
        nestedThread["turns"][0]["items"][2]["data"]["content"].push_back(
            frontend::Json{{"type", "future_input"}, {"opaqueFutureValue", 99}});
        auto decoded = model::decodeThreadReadStateEffectThread(nestedThread, model::FrontendSequence{12});
        if (!decoded) {
            result.expectTrue(false,
                              "authoritative thread.read body decodes through the frozen nested ThreadState contract: " +
                                  decoded.error().path + ": " + decoded.error().message);
            result.expectTrue(false, "authoritative thread.read replacement applies transactionally");
            return;
        }
        const auto decodedTurn =
            std::find_if(decoded.value().turns.begin(), decoded.value().turns.end(), [](const model::TurnState& value) {
                return value.id.value() == "turn-metadata";
            });
        const auto decodedItem =
            std::find_if(decoded.value().items.begin(), decoded.value().items.end(), [](const model::ThreadItem& value) {
                return model::itemData(value).id.value() == "item-metadata";
            });
        const auto decodedUserMessage =
            std::find_if(decoded.value().items.begin(), decoded.value().items.end(), [](const model::ThreadItem& value) {
                return model::itemData(value).id.value() == "item-user-message";
            });
        const auto decodedEscapedCommand =
            std::find_if(decoded.value().items.begin(), decoded.value().items.end(), [](const model::ThreadItem& value) {
                return model::itemData(value).id.value() == "item-escaped-command";
            });
        const auto decodedLongUserMessage =
            std::find_if(decoded.value().items.begin(), decoded.value().items.end(), [](const model::ThreadItem& value) {
                return model::itemData(value).id.value() == "item-long-user-message";
            });
        const auto decodedFutureItem = std::find_if(
            decoded.value().legacyItems.begin(), decoded.value().legacyItems.end(), [](const model::LegacyItemCompatibility& value) {
                return value.value.id.value() == "item-future";
            });
        const bool decodedMetadata =
            decoded.value().thread.stamp.generation == 7 && decoded.value().thread.stamp.freshness == model::Freshness::Current &&
            decoded.value().thread.safeDetails.json().value("ephemeral", true) == false &&
            decoded.value().thread.safeDetails.json().value("archived", false) == true &&
            decoded.value().thread.safeDetails.json().at("executionConfiguration") == executionConfiguration &&
            decodedTurn != decoded.value().turns.end() && decodedTurn->stamp.generation == 8 && decodedTurn->connectionInvalidated &&
            decodedTurn->plan && decodedTurn->plan->steps.size() == 1 && decodedTurn->plan->steps.front().step == "Inspect" &&
            decodedTurn->plan->steps.front().status == "completed" &&
            decodedTurn->safeDetails.json().at("effectiveExecutionConfiguration") == executionConfiguration &&
            decodedItem != decoded.value().items.end() && model::itemData(*decodedItem).generation == 9 &&
            model::itemData(*decodedItem).freshness == model::Freshness::Stale && model::itemData(*decodedItem).connectionInvalidated &&
            decodedUserMessage != decoded.value().items.end() && model::itemData(*decodedUserMessage).userMessage &&
            model::itemData(*decodedUserMessage).userMessage->textParts == std::vector<std::string>{"first", "second"} &&
            model::itemData(*decodedUserMessage).safeDetails &&
            model::itemData(*decodedUserMessage).safeDetails->json().at("content").size() == 1 &&
            model::itemData(*decodedUserMessage).safeDetails->json().at("content").at(0).at("type") == "future_input" &&
            model::itemData(*decodedUserMessage).safeDetails->json().at("content").at(0).at("opaqueFutureValue") == 99 &&
            decodedLongUserMessage != decoded.value().items.end() && model::itemData(*decodedLongUserMessage).userMessage &&
            model::itemData(*decodedLongUserMessage).userMessage->text == longUserText &&
            model::itemData(*decodedLongUserMessage).userMessage->textParts == std::vector<std::string>{longUserText} &&
            (!model::itemData(*decodedLongUserMessage).safeDetails ||
             !model::itemData(*decodedLongUserMessage).safeDetails->json().contains("content")) &&
            decodedEscapedCommand != decoded.value().items.end() &&
            model::itemData(*decodedEscapedCommand).commandOutput == std::optional<std::string>{std::string(20'000, '\\')} &&
            decodedFutureItem != decoded.value().legacyItems.end() && decodedFutureItem->discriminator == "future_super_item" &&
            decodedFutureItem->value.safeDetails && decodedFutureItem->value.safeDetails->json().value("opaqueFutureValue", 0) == 42 &&
            decodedFutureItem->value.threadId == std::optional<model::ThreadIdentity>{model::ThreadIdentity{"thread-metadata"}} &&
            decodedFutureItem->value.turnId == std::optional<model::TurnIdentity>{model::TurnIdentity{"turn-metadata"}} &&
            decodedFutureItem->sourceIndex == 1 && decodedFutureItem->value.sourceIndex == 1 &&
            decodedFutureItem->value.extensions.json().value("futureExtensionValue", 0) == 73;
        result.expectTrue(decodedMetadata, "authoritative thread.read body decodes through the frozen nested ThreadState contract");

        frontend::Json mismatchedTurnParent = encodedThread;
        mismatchedTurnParent["turns"][0]["threadId"] = "different-thread";
        const auto rejectedTurnParent = model::decodeThreadReadStateEffectThread(mismatchedTurnParent, model::FrontendSequence{12});
        frontend::Json mismatchedItemThreadParent = encodedThread;
        mismatchedItemThreadParent["turns"][0]["items"][0]["threadId"] = "different-thread";
        const auto rejectedItemThreadParent =
            model::decodeThreadReadStateEffectThread(mismatchedItemThreadParent, model::FrontendSequence{12});
        frontend::Json mismatchedItemTurnParent = encodedThread;
        mismatchedItemTurnParent["turns"][0]["items"][0]["turnId"] = "different-turn";
        const auto rejectedItemTurnParent = model::decodeThreadReadStateEffectThread(mismatchedItemTurnParent, model::FrontendSequence{12});
        frontend::Json mismatchedExtensionThreadParent = encodedThread;
        mismatchedExtensionThreadParent["turns"][0]["items"][0]["extensions"]["threadId"] = "different-thread";
        const auto rejectedExtensionThreadParent =
            model::decodeThreadReadStateEffectThread(mismatchedExtensionThreadParent, model::FrontendSequence{12});
        frontend::Json mismatchedExtensionTurnParent = encodedThread;
        mismatchedExtensionTurnParent["turns"][0]["items"][0]["extensions"]["turnId"] = "different-turn";
        const auto rejectedExtensionTurnParent =
            model::decodeThreadReadStateEffectThread(mismatchedExtensionTurnParent, model::FrontendSequence{12});
        frontend::Json mismatchedFutureExtensionParent = encodedThread;
        mismatchedFutureExtensionParent["turns"][0]["items"][1]["extensions"]["threadId"] = "different-thread";
        const auto rejectedFutureExtensionParent =
            model::decodeThreadReadStateEffectThread(mismatchedFutureExtensionParent, model::FrontendSequence{12});
        result.expectTrue(
            !rejectedTurnParent && rejectedTurnParent.error().path == "/thread/turns/0/threadId" && !rejectedItemThreadParent &&
                rejectedItemThreadParent.error().path == "/thread/turns/0/items/0/threadId" && !rejectedItemTurnParent &&
                rejectedItemTurnParent.error().path == "/thread/turns/0/items/0/turnId" && !rejectedExtensionThreadParent &&
                rejectedExtensionThreadParent.error().path == "/thread/turns/0/items/0/extensions/threadId" &&
                !rejectedExtensionTurnParent && rejectedExtensionTurnParent.error().path == "/thread/turns/0/items/0/extensions/turnId" &&
                !rejectedFutureExtensionParent &&
                rejectedFutureExtensionParent.error().path == "/thread/turns/0/items/1/extensions/threadId",
            "authoritative thread.read rejects direct and flattened-extension descendants outside their containing thread "
            "and turn for both known and future item families");

        model::CanonicalSnapshot mergeCandidate;
        mergeCandidate.threads.emplace_back(model::ThreadIdentity{"thread-metadata"});
        mergeCandidate.threads.front().fullyLoaded = true;
        mergeCandidate.turns.emplace_back(model::TurnIdentity{"retained-turn"}, model::ThreadIdentity{"thread-metadata"});
        model::ItemData retainedItem{model::ItemIdentity{"retained-item"},
                                     model::ThreadIdentity{"thread-metadata"},
                                     model::TurnIdentity{"retained-turn"}};
        retainedItem.sourceIndex = 0;
        mergeCandidate.items.emplace_back(model::AgentMessageItem{std::move(retainedItem)});
        model::ItemData retainedFuture{model::ItemIdentity{"retained-future"},
                                       model::ThreadIdentity{"thread-metadata"},
                                       model::TurnIdentity{"retained-turn"}};
        retainedFuture.sourceIndex = 1;
        mergeCandidate.legacyItems.push_back(
            {std::move(retainedFuture), "retained_future_item", 1, "/thread/turns/0/items/1"});
        model::ThreadUpsertedOccurrence mergeUpdate = decoded.value();
        mergeUpdate.thread.fullyLoaded = false;
        mergeUpdate.authority = model::ThreadUpsertAuthority::MergeApplyCompleteness;
        model::OccurrenceIdentity mergeIdentity{model::FrontendSequence{12},
                                                model::OccurrenceGroupIdentity{"thread-read-merge"},
                                                0,
                                                1,
                                                model::SourceStamp{"thread-read-merge-source"}};
        mergeIdentity.threadId = model::ThreadIdentity{"thread-metadata"};
        auto mergeOccurrence = model::makeOccurrence(std::move(mergeIdentity), std::move(mergeUpdate));
        const auto mergeApplied =
            mergeOccurrence ? model::applyOccurrence(mergeCandidate, mergeOccurrence.value())
                            : model::ModelResult<bool>{model::ModelError{model::ModelErrorCode::InvalidShape,
                                                                        "/thread",
                                                                        "merge occurrence construction failed"}};
        const auto mergedFuture = std::find_if(
            mergeCandidate.legacyItems.begin(), mergeCandidate.legacyItems.end(), [](const model::LegacyItemCompatibility& value) {
                return value.value.id.value() == "item-future";
            });
        const auto mergedUser = std::find_if(mergeCandidate.items.begin(), mergeCandidate.items.end(), [](const model::ThreadItem& value) {
            return model::itemData(value).id.value() == "item-user-message";
        });
        result.expectTrue(
            mergeApplied && !mergeCandidate.threads.front().fullyLoaded && mergeCandidate.turns.size() == 2 &&
                std::any_of(mergeCandidate.items.begin(), mergeCandidate.items.end(), [](const model::ThreadItem& value) {
                    return model::itemData(value).id.value() == "retained-item";
                }) &&
                std::any_of(mergeCandidate.legacyItems.begin(),
                            mergeCandidate.legacyItems.end(),
                            [](const model::LegacyItemCompatibility& value) {
                                return value.value.id.value() == "retained-future";
                            }) &&
                mergedFuture != mergeCandidate.legacyItems.end() && mergedUser != mergeCandidate.items.end() &&
                mergedFuture->sourceIndex < *model::itemData(*mergedUser).sourceIndex,
            "merge-authoritative thread.read retains omitted descendants and upserts typed and future items in source order");

        model::CanonicalSnapshot candidate;
        candidate.threads.emplace_back(model::ThreadIdentity{"thread-metadata"});
        candidate.turns.emplace_back(model::TurnIdentity{"old-turn"}, model::ThreadIdentity{"thread-metadata"});
        model::ItemData oldItem{model::ItemIdentity{"old-item"},
                                model::ThreadIdentity{"thread-metadata"},
                                model::TurnIdentity{"old-turn"}};
        candidate.items.emplace_back(model::AgentMessageItem{std::move(oldItem)});
        model::OccurrenceIdentity identity{model::FrontendSequence{12},
                                           model::OccurrenceGroupIdentity{"thread-read-metadata"},
                                           0,
                                           1,
                                           model::SourceStamp{"thread-read-metadata-source"}};
        identity.threadId = model::ThreadIdentity{"thread-metadata"};
        auto occurrence = model::makeOccurrence(std::move(identity), std::move(decoded).value());
        const auto applied = occurrence ? model::applyOccurrence(candidate, occurrence.value())
                                        : model::ModelResult<bool>{model::ModelError{model::ModelErrorCode::InvalidShape,
                                                                                    "/thread",
                                                                                    "occurrence construction failed"}};
        result.expectTrue(
            applied && candidate.turns.size() == 1 && candidate.turns.front().id.value() == "turn-metadata" &&
                std::none_of(candidate.items.begin(), candidate.items.end(), [](const model::ThreadItem& value) {
                    return model::itemData(value).id.value() == "old-item";
                }) &&
                std::any_of(candidate.items.begin(), candidate.items.end(), [](const model::ThreadItem& value) {
                    return model::itemData(value).id.value() == "item-user-message";
                }) &&
                candidate.legacyItems.size() == 1 && candidate.legacyItems.front().value.id.value() == "item-future" &&
                candidate.legacyItems.front().sourceIndex == 1,
            "authoritative thread.read replacement applies transactionally, preserves future items in order, and deletes stale descendants");
    }

} // namespace

int main() {
    tests::support::TestResult result;
    testCoalescingAndBounds(result);
    testThreadReadSnapshotPreservesCanonicalMetadata(result);
    return result.processResult();
}
