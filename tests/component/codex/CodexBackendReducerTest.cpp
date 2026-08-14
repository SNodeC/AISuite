/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/backend/Reducer.h"
#include "ai/openai/codex/backend/Snapshot.h"
#include "ai/openai/codex/backend/detail/PreserveUnmodeledTypedEvent.h"
#include "ai/openai/codex/backend/internal/RetentionCapacityInstrumentation.h"
#include "ai/openai/codex/detail/ProtocolSurfaceRegistry.h"
#include "support/TestResult.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace backend = ai::openai::codex::backend;
    namespace typed = ai::openai::codex::typed;

    using ai::openai::codex::Error;
    using ai::openai::codex::Json;
    using ai::openai::codex::ServerRequestId;
    using ai::openai::codex::ServerRequestToken;

    static_assert(requires(backend::BackendState state) {
        state.accounts.loginCancellation;
        state.accounts.loginStart;
        state.accounts.rateLimitRead;
        state.accounts.accountRead;
        state.accounts.usage;
        state.accounts.workspaceMessages;
        state.configuration.configuration;
        state.configuration.requirements;
        state.configuration.experimentalFeatures;
        state.configuration.lastWrite;
        state.configuration.experimentalFeatureEnablement;
        state.models.list;
        state.models.providerCapabilities;
        state.conversations.latestGoal;
        state.conversations.latestGoalClear;
        state.conversations.latestGoalSet;
        state.conversations.latestUnsubscribe;
        state.conversations.loadedThreads;
        state.reviews.permissionProfiles;
        state.reviews.latestReview;
        state.integrations.appList;
        state.integrations.externalAgentDetection;
        state.integrations.externalAgentImport;
        state.integrations.externalAgentImportHistories;
        state.integrations.hooks;
        state.integrations.marketplaceAdd;
        state.integrations.marketplaceRemove;
        state.integrations.marketplaceUpgrade;
        state.pluginsAndSkills.pluginInstall;
        state.pluginsAndSkills.installedPlugins;
        state.pluginsAndSkills.plugins;
        state.pluginsAndSkills.pluginDetail;
        state.pluginsAndSkills.pluginShares;
        state.pluginsAndSkills.pluginShareCheckout;
        state.pluginsAndSkills.pluginShareSave;
        state.pluginsAndSkills.pluginShareUpdateTargets;
        state.pluginsAndSkills.pluginSkill;
        state.pluginsAndSkills.skills;
        state.pluginsAndSkills.skillsConfigWrite;
        state.pluginsAndSkills.extraRoots;
        state.mcp.oauthStart;
        state.mcp.statusListResponse;
        state.platform.windowsReadiness;
    });

    typed::ItemMetadata metadata(const std::string& threadId, const std::string& turnId, const std::string& itemId) {
        typed::ItemMetadata result;
        result.id = typed::ItemId{itemId};
        result.threadId = typed::ThreadId{threadId};
        result.turnId = typed::TurnId{turnId};
        result.raw = Json{{"id", itemId}, {"threadId", threadId}, {"turnId", turnId}};
        return result;
    }

    typed::ThreadItem agentItem(const std::string& threadId, const std::string& turnId, const std::string& itemId, std::string text = {}) {
        typed::AgentMessageThreadItem item;
        item.metadata = metadata(threadId, turnId, itemId);
        item.text = std::move(text);
        return item;
    }

    typed::UserMessageThreadItem userMessageItem(const std::string& threadId,
                                                 const std::string& turnId,
                                                 const std::string& itemId,
                                                 Json content,
                                                 Json raw,
                                                 std::optional<std::string> clientId = std::nullopt) {
        typed::UserMessageThreadItem item;
        item.metadata = metadata(threadId, turnId, itemId);
        item.metadata.raw = std::move(raw);
        if (!item.metadata.raw.contains("content")) {
            item.metadata.raw["content"] = content;
        }
        if (clientId) {
            item.clientId = typed::ClientUserMessageId{std::move(*clientId)};
        }
        for (Json& entry : content) {
            std::optional<std::string> type;
            if (const auto discriminator = entry.find("type"); discriminator != entry.end() && discriminator->is_string()) {
                type = discriminator->get<std::string>();
            }
            item.content.emplace_back(typed::UnknownTurnInput{std::move(type), std::move(entry), std::nullopt});
        }
        return item;
    }

    typed::ThreadItem reasoningItem(const std::string& threadId,
                                    const std::string& turnId,
                                    const std::string& itemId,
                                    std::string text = {},
                                    std::string summary = {}) {
        typed::ReasoningThreadItem item;
        item.metadata = metadata(threadId, turnId, itemId);
        if (!text.empty()) {
            item.content = std::vector<std::string>{std::move(text)};
        }
        if (!summary.empty()) {
            item.summary = std::vector<std::string>{std::move(summary)};
        }
        return item;
    }

    typed::ThreadItem commandItem(const std::string& threadId,
                                  const std::string& turnId,
                                  const std::string& itemId,
                                  std::optional<std::string> output = std::nullopt) {
        typed::CommandExecutionThreadItem item;
        item.metadata = metadata(threadId, turnId, itemId);
        item.command = "printf test";
        item.cwd = typed::PathString{"/tmp/project"};
        item.status = typed::CommandExecutionStatus::inProgress();
        item.aggregatedOutput = std::move(output);
        return item;
    }

    typed::Turn turn(const std::string& threadId,
                     const std::string& turnId,
                     typed::TurnStatus status = typed::TurnStatus::inProgress(),
                     std::vector<typed::ThreadItem> items = {}) {
        typed::Turn result;
        result.id = typed::TurnId{turnId};
        result.threadId = typed::ThreadId{threadId};
        result.status = std::move(status);
        result.itemsView = typed::TurnItemsView::full();
        result.items = std::move(items);
        result.raw = Json{{"id", turnId}, {"threadId", threadId}};
        return result;
    }

    typed::Thread thread(const std::string& threadId, std::vector<typed::Turn> turns = {}) {
        typed::Thread result;
        result.id = typed::ThreadId{threadId};
        result.title = "Thread " + threadId;
        result.cwd = typed::AbsolutePath{"/tmp/project"};
        result.model = typed::ModelId{"gpt-5"};
        result.modelProvider = "openai";
        result.preview = "preview " + threadId;
        result.status = typed::IdleThreadStatus{Json{{"type", "idle"}}, {}};
        result.createdAt = 1;
        result.updatedAt = 2;
        result.turns = std::move(turns);
        result.raw = Json{{"id", threadId}, {"futureThreadField", true}};
        return result;
    }

    const backend::TurnState* findTurn(const backend::BackendState& state, const std::string& threadId, const std::string& turnId) {
        return backend::findTurn(state, typed::ThreadId{threadId}, typed::TurnId{turnId});
    }

    const backend::ItemState*
    findItem(const backend::BackendState& state, const std::string& threadId, const std::string& turnId, const std::string& itemId) {
        return backend::findItem(state, typed::ThreadId{threadId}, typed::TurnId{turnId}, typed::ItemId{itemId});
    }

    template <typename Notification>
    typed::Event notificationEvent() {
        Notification notification{};
        notification.raw = Json{{"params", Json::object()}};
        return typed::Event{std::move(notification)};
    }

    template <typename Request>
    Request deferredRequest(std::uint64_t occurrence) {
        return Request{
            ServerRequestId{std::string{"deferred-"} + std::to_string(occurrence)}, ServerRequestToken{occurrence}, {}, Json::object(), {}};
    }

    template <std::size_t Index>
    bool eventAlternativeTranslates(const backend::Reducer& reducer) {
        using EventValue = std::variant_alternative_t<Index, typed::Event>;
        if constexpr (!std::is_default_constructible_v<EventValue>) {
            return false;
        } else {
            EventValue value{};
            if constexpr (requires(EventValue& candidate) { candidate.raw = Json::object(); }) {
                value.raw = Json{{"params", Json::object()}};
            }
            return !reducer.translate(typed::Event{std::move(value)}).empty();
        }
    }

    template <std::size_t... Indices>
    bool allEventAlternativesTranslate(const backend::Reducer& reducer, std::index_sequence<Indices...>) {
        return (eventAlternativeTranslates<Indices>(reducer) && ...);
    }

    void testInitialStateAndLifecycle(tests::support::TestResult& result) {
        backend::BackendState state;
        backend::Reducer reducer;

        const backend::Snapshot first = backend::makeSnapshot(state);
        const backend::Snapshot second = backend::makeSnapshot(state);
        result.expectTrue(first == second, "unchanged empty state produces equal deterministic snapshots");
        result.expectTrue(first.provider.lifecycle == backend::ProviderLifecycle::Stopped && first.threads.empty() &&
                              first.pendingRequests.empty() && first.sessions.empty() && first.sequence.value() == 0,
                          "initial snapshot is stopped and contains no domain entities");

        const auto transition = [&reducer,
                                 &state](backend::ProviderLifecycle lifecycle, std::optional<Error> error, std::uint64_t generation) {
            backend::ProviderState provider = state.provider;
            provider.lifecycle = lifecycle;
            provider.generation = generation;
            if (error.has_value()) {
                provider.lastError = std::move(error);
            } else if (lifecycle == backend::ProviderLifecycle::Ready) {
                provider.lastError.reset();
            }
            return reducer.apply(state, backend::ProviderLifecycleChanged{std::move(provider)});
        };

        const backend::Reduction starting = transition(backend::ProviderLifecycle::Starting, std::nullopt, 1);
        result.expectTrue(starting.changed && !starting.flushImmediately &&
                              state.provider.lifecycle == backend::ProviderLifecycle::Starting,
                          "starting lifecycle transition changes canonical state");

        const Error error{Error::Category::Transport, 71, "connection failed"};
        const backend::Reduction failed = transition(backend::ProviderLifecycle::Failed, error, 1);
        result.expectTrue(failed.changed && failed.flushImmediately && state.provider.lastError &&
                              state.provider.lastError->message == "connection failed",
                          "failure is retained and requests an immediate frontend flush");

        transition(backend::ProviderLifecycle::Starting, std::nullopt, 2);
        result.expectTrue(state.provider.lastError.has_value(), "a prior lifecycle error remains visible while recovery is starting");
        transition(backend::ProviderLifecycle::Ready, std::nullopt, 2);
        result.expectTrue(state.provider.lifecycle == backend::ProviderLifecycle::Ready && !state.provider.lastError,
                          "ready after restart clears the prior lifecycle error");

        backend::ReducerOptions options;
        options.retainedDiagnostics = 2;
        backend::Reducer boundedReducer(options);
        boundedReducer.apply(state, backend::DiagnosticReceived{"one"});
        boundedReducer.apply(state, backend::DiagnosticReceived{"two"});
        boundedReducer.apply(state, backend::DiagnosticReceived{"three"});
        result.expectTrue(state.diagnostics.received == 3 && state.diagnostics.recent == std::vector<std::string>({"two", "three"}),
                          "diagnostic total is monotonic while the retained detail list is bounded");
    }

    void testThreadAndTurnHydration(tests::support::TestResult& result) {
        backend::BackendState state;
        backend::Reducer reducer;

        typed::Thread started = thread("thread-start");
        reducer.apply(state, backend::ThreadUpserted{started, backend::EntityLoad::Summary});
        result.expectTrue(state.threads.size() == 1 && state.threadOrder.size() == 1 &&
                              state.threads.at("thread-start").thread.title == "Thread thread-start" &&
                              !state.threads.at("thread-start").fullyLoaded,
                          "thread/start result performs a summary ID upsert");

        typed::Thread resumed = started;
        resumed.title = "Resumed title";
        reducer.apply(state, backend::ThreadUpserted{resumed, backend::EntityLoad::Summary});
        result.expectTrue(state.threads.size() == 1 && state.threadOrder.size() == 1 &&
                              state.threads.at("thread-start").thread.title == "Resumed title",
                          "thread/resume result replaces the typed thread without duplicating its order entry");

        typed::ThreadListResponse firstPage;
        firstPage.data = {thread("thread-list-b"), thread("thread-list-a")};
        firstPage.nextCursor = std::string{"cursor-2"};
        firstPage.backwardsCursor = std::string{"cursor-before"};
        reducer.apply(state, backend::ThreadListUpdated{firstPage, std::nullopt, true});
        result.expectTrue(state.threadList.hasLoadedPage && !state.threadList.complete && state.threadList.pagesLoaded == 1 &&
                              state.threadList.nextCursor == "cursor-2" && state.threads.size() == 3,
                          "first thread/list page merges threads and retains pagination state");

        typed::ThreadListResponse secondPage;
        secondPage.data = {thread("thread-list-c"), thread("thread-list-b")};
        secondPage.backwardsCursor = std::string{"cursor-1"};
        reducer.apply(state, backend::ThreadListUpdated{secondPage, std::string("cursor-2"), false});
        result.expectTrue(state.threadList.complete && state.threadList.pagesLoaded == 2 && !state.threadList.nextCursor &&
                              state.threads.size() == 4,
                          "subsequent thread/list page uses ID merge semantics and marks terminal pagination complete");

        const typed::Turn readTurnA = turn("thread-start", "turn-z");
        const typed::Turn readTurnB = turn("thread-start", "turn-a", typed::TurnStatus::completed());
        typed::Thread read = thread("thread-start", {readTurnA, readTurnB});
        read.title = "Fully read";
        reducer.apply(state, backend::ThreadUpserted{read, backend::EntityLoad::Full});
        const backend::ThreadState& hydrated = state.threads.at("thread-start");
        result.expectTrue(hydrated.fullyLoaded && hydrated.turns.size() == 2 && hydrated.turnOrder.size() == 2 &&
                              hydrated.turnOrder[0].value == "turn-z" && hydrated.turnOrder[1].value == "turn-a",
                          "thread/read hydrates turns fully in deterministic server order");

        typed::Turn turnUpdate = readTurnA;
        turnUpdate.status = typed::TurnStatus::completed();
        reducer.apply(state, backend::TurnUpserted{turnUpdate});
        result.expectTrue(hydrated.turns.at("turn-z").terminal && !hydrated.turns.at("turn-z").active && hydrated.turnOrder.size() == 2,
                          "turn/start or notification upserts status without duplicating turn order");

        const backend::Snapshot snapshot = backend::makeSnapshot(state);
        result.expectTrue(snapshot.threads.size() == 4 && snapshot.threads.front().id == "thread-start" &&
                              snapshot.threads.front().turns.size() == 2 && snapshot.threads.front().turns[0].id == "turn-z" &&
                              snapshot.threads.front().turns[1].id == "turn-a",
                          "snapshot preserves first-seen thread, turn, and item ordering instead of map key ordering");
    }

    void testStatusOnlyPlaceholderParity(tests::support::TestResult& result) {
        backend::BackendState state;
        backend::Reducer reducer;

        const Json statusEnvelope = {
            {"method", "thread/status/changed"},
            {"params", {{"threadId", "thread-status-only"}, {"status", {{"type", "active"}, {"activeFlags", Json::array()}}}}},
        };
        const typed::ThreadStatus activeStatus =
            typed::ActiveThreadStatus{{}, statusEnvelope.at("params").at("status"), {}};
        const std::vector<backend::BackendEvent> statusEvents =
            reducer.translate(typed::Event{typed::ThreadStatusChanged{
                typed::ThreadId{"thread-status-only"}, activeStatus, statusEnvelope}});
        const auto* statusUpdated =
            statusEvents.size() == 1 ? std::get_if<backend::ThreadStatusUpdated>(&statusEvents.front()) : nullptr;
        result.expectTrue(statusUpdated && statusUpdated->threadId.value == "thread-status-only" &&
                              typed::threadStatusDiscriminator(statusUpdated->status) == "active",
                          "thread/status/changed retains its existing modeled status-update translation");
        if (statusUpdated) {
            reducer.apply(state, *statusUpdated);
        }
        const backend::Snapshot statusOnlySnapshot = backend::makeSnapshot(state);
        result.expectTrue(statusOnlySnapshot.threads.size() == 1 &&
                              statusOnlySnapshot.threads.front().id == "thread-status-only" &&
                              statusOnlySnapshot.threads.front().status == "active" &&
                              !statusOnlySnapshot.threads.front().cwd &&
                              !statusOnlySnapshot.threads.front().modelProvider &&
                              !statusOnlySnapshot.threads.front().preview &&
                              !statusOnlySnapshot.threads.front().createdAt &&
                              !statusOnlySnapshot.threads.front().updatedAt,
                          "a status-only placeholder exposes the modeled status without leaking defaults for unknown thread fields");

        typed::Thread collision = thread("thread-marker-collision");
        collision.raw["backendPlaceholder"] = true;
        reducer.apply(state, backend::ThreadUpserted{collision, backend::EntityLoad::Summary});
        const backend::Snapshot collisionSnapshot = backend::makeSnapshot(state);
        const auto collisionThread =
            std::find_if(collisionSnapshot.threads.begin(),
                         collisionSnapshot.threads.end(),
                         [](const backend::ThreadSnapshot& candidate) {
                             return candidate.id == "thread-marker-collision";
                         });
        result.expectTrue(collisionThread != collisionSnapshot.threads.end() && collisionThread->cwd == "[redacted]" &&
                              collisionThread->modelProvider == "openai" && collisionThread->preview == "preview thread-marker-collision" &&
                              collisionThread->status == "idle" && collisionThread->createdAt == 1 && collisionThread->updatedAt == 2,
                          "an unknown wire field named backendPlaceholder cannot collide with the exact internal sentinel");
    }

    void testItemsAndHighVolumeDeltas(tests::support::TestResult& result) {
        backend::BackendState state;
        backend::Reducer reducer;
        reducer.apply(state, backend::TurnUpserted{turn("thread-delta", "turn-delta")});

        const backend::Reduction agentStart = reducer.apply(state,
                                                            backend::ItemUpserted{typed::ThreadId{"thread-delta"},
                                                                                  typed::TurnId{"turn-delta"},
                                                                                  agentItem("thread-delta", "turn-delta", "item-agent"),
                                                                                  backend::ItemLifecycle::Started,
                                                                                  10});
        reducer.apply(state,
                      backend::ItemUpserted{typed::ThreadId{"thread-delta"},
                                            typed::TurnId{"turn-delta"},
                                            reasoningItem("thread-delta", "turn-delta", "item-reasoning"),
                                            backend::ItemLifecycle::Started,
                                            11});
        reducer.apply(state,
                      backend::ItemUpserted{typed::ThreadId{"thread-delta"},
                                            typed::TurnId{"turn-delta"},
                                            commandItem("thread-delta", "turn-delta", "item-command"),
                                            backend::ItemLifecycle::Started,
                                            12});
        result.expectTrue(agentStart.changed && !agentStart.flushImmediately,
                          "item start changes canonical state without forcing a terminal flush");

        std::string expectedAgent;
        std::string expectedReasoning;
        std::string expectedCommand;
        for (std::size_t index = 0; index < 1000; ++index) {
            const std::string agentDelta = "a" + std::to_string(index % 10);
            const std::string reasoningDelta = "r" + std::to_string(index % 10);
            const std::string commandDelta = "c" + std::to_string(index % 10);
            expectedAgent += agentDelta;
            expectedReasoning += reasoningDelta;
            expectedCommand += commandDelta;
            reducer.apply(state,
                          backend::ItemContentChanged{typed::ThreadId{"thread-delta"},
                                                      typed::TurnId{"turn-delta"},
                                                      typed::ItemId{"item-agent"},
                                                      backend::ItemContentChanged::Kind::AgentText,
                                                      agentDelta,
                                                      std::nullopt});
            reducer.apply(state,
                          backend::ItemContentChanged{typed::ThreadId{"thread-delta"},
                                                      typed::TurnId{"turn-delta"},
                                                      typed::ItemId{"item-reasoning"},
                                                      backend::ItemContentChanged::Kind::ReasoningText,
                                                      reasoningDelta,
                                                      static_cast<std::int64_t>(index)});
            reducer.apply(state,
                          backend::ItemContentChanged{typed::ThreadId{"thread-delta"},
                                                      typed::TurnId{"turn-delta"},
                                                      typed::ItemId{"item-command"},
                                                      backend::ItemContentChanged::Kind::CommandOutput,
                                                      commandDelta,
                                                      std::nullopt});
        }

        const backend::ItemState* accumulatedAgent = findItem(state, "thread-delta", "turn-delta", "item-agent");
        const backend::ItemState* accumulatedReasoning = findItem(state, "thread-delta", "turn-delta", "item-reasoning");
        const backend::ItemState* accumulatedCommand = findItem(state, "thread-delta", "turn-delta", "item-command");
        result.expectTrue(accumulatedAgent && accumulatedAgent->agentText == expectedAgent,
                          "1,000 agent-message deltas accumulate exact final text in canonical state");
        result.expectTrue(accumulatedReasoning && accumulatedReasoning->reasoningText == expectedReasoning,
                          "1,000 reasoning deltas accumulate exact final reasoning without crossing item boundaries");
        result.expectTrue(accumulatedCommand && accumulatedCommand->commandOutput == expectedCommand,
                          "1,000 command-output deltas accumulate exact final output without crossing item boundaries");

        reducer.apply(state,
                      backend::ItemContentChanged{typed::ThreadId{"thread-delta"},
                                                  typed::TurnId{"turn-delta"},
                                                  typed::ItemId{"item-reasoning"},
                                                  backend::ItemContentChanged::Kind::ReasoningSummary,
                                                  "summary",
                                                  0});
        result.expectTrue(findItem(state, "thread-delta", "turn-delta", "item-reasoning")->reasoningSummary == "summary",
                          "reasoning summaries are accumulated independently from reasoning text");

        const backend::Reduction agentComplete =
            reducer.apply(state,
                          backend::ItemUpserted{typed::ThreadId{"thread-delta"},
                                                typed::TurnId{"turn-delta"},
                                                agentItem("thread-delta", "turn-delta", "item-agent", expectedAgent),
                                                backend::ItemLifecycle::Completed,
                                                20});
        result.expectTrue(agentComplete.flushImmediately &&
                              findItem(state, "thread-delta", "turn-delta", "item-agent")->lifecycle == backend::ItemLifecycle::Completed &&
                              findItem(state, "thread-delta", "turn-delta", "item-agent")->completedAtMs == 20,
                          "item completion preserves final content and forces an immediate flush");

        const backend::TurnState* deltaTurn = findTurn(state, "thread-delta", "turn-delta");
        result.expectTrue(deltaTurn && deltaTurn->itemOrder.size() == 3 && deltaTurn->itemOrder[0].value == "item-agent" &&
                              deltaTurn->itemOrder[1].value == "item-reasoning" && deltaTurn->itemOrder[2].value == "item-command",
                          "independent items remain in deterministic first-seen order");

        const backend::Snapshot snapshot = backend::makeSnapshot(state);
        result.expectTrue(snapshot.threads.size() == 1 && snapshot.threads[0].turns.size() == 1 &&
                              snapshot.threads[0].turns[0].items.size() == 3 &&
                              snapshot.threads[0].turns[0].items[0].agentText == expectedAgent &&
                              snapshot.threads[0].turns[0].items[1].reasoningText == expectedReasoning &&
                              snapshot.threads[0].turns[0].items[2].commandOutput == expectedCommand,
                          "snapshot exposes exact accumulated content for each item without merging entities");
    }

    void testCompletionFailureAndAuxiliaryUpdates(tests::support::TestResult& result) {
        backend::BackendState state;
        backend::Reducer reducer;

        typed::Turn completed = turn("thread-terminal", "turn-completed", typed::TurnStatus::completed());
        const backend::Reduction completedReduction = reducer.apply(state, backend::TurnCompleted{completed});
        const backend::TurnState* completedState = findTurn(state, "thread-terminal", "turn-completed");
        result.expectTrue(completedReduction.flushImmediately && completedState && completedState->terminal && !completedState->active,
                          "turn completion marks terminal state and flushes immediately");

        typed::Turn failed = turn("thread-terminal", "turn-failed", typed::TurnStatus::failed());
        const Json failure = {{"message", "model failed"}, {"future", 9}};
        const backend::Reduction failedReduction = reducer.apply(state, backend::TurnFailed{failed, failure});
        const backend::TurnState* failedState = findTurn(state, "thread-terminal", "turn-failed");
        result.expectTrue(failedReduction.flushImmediately && failedState && failedState->terminal && failedState->failure == failure,
                          "turn failure retains structured failure information and flushes immediately");

        reducer.apply(state,
                      backend::TokenUsageUpdated{
                          typed::ThreadId{"thread-terminal"}, typed::TurnId{"turn-failed"}, Json{{"total", {{"inputTokens", 17}}}}});
        reducer.apply(state,
                      backend::ModelRerouted{typed::ThreadId{"thread-terminal"},
                                             typed::TurnId{"turn-failed"},
                                             typed::ModelId{"gpt-old"},
                                             typed::ModelId{"gpt-new"},
                                             "capacity"});
        failedState = findTurn(state, "thread-terminal", "turn-failed");
        result.expectTrue(failedState && failedState->tokenUsage && failedState->modelReroutes.size() == 1 &&
                              failedState->modelReroutes[0].to.value == "gpt-new",
                          "token usage and model rerouting are retained on the owning turn");

        typed::FileChangeThreadItem file;
        file.metadata = metadata("thread-terminal", "turn-failed", "file-item");
        file.status = typed::PatchApplyStatus::inProgress();
        reducer.apply(state,
                      backend::ItemUpserted{typed::ThreadId{"thread-terminal"},
                                            typed::TurnId{"turn-failed"},
                                            typed::ThreadItem{file},
                                            backend::ItemLifecycle::Started,
                                            std::nullopt});
        const Json changes = Json::array({Json{{"path", "a.cpp"}, {"kind", "update"}}});
        reducer.apply(state,
                      backend::FileChangeUpdated{
                          typed::ThreadId{"thread-terminal"}, typed::TurnId{"turn-failed"}, typed::ItemId{"file-item"}, changes});
        const backend::ItemState* fileState = findItem(state, "thread-terminal", "turn-failed", "file-item");
        result.expectTrue(fileState && fileState->extensions.at("fileChanges") == changes,
                          "file-change updates remain associated with the correct item");
    }

    void testUserMessageLifecycle(tests::support::TestResult& result) {
        backend::BackendState state;
        backend::Reducer reducer;

        const Json startedContent = Json::array({Json{{"type", "text"}, {"text", "Answer just with OK!"}}});
        const Json startedRaw = {
            {"type", "userMessage"}, {"id", "user-item"}, {"clientId", nullptr}, {"content", startedContent}, {"future", 1}};
        const typed::UserMessageThreadItem startedItem =
            userMessageItem("thread-user", "turn-user", "user-item", startedContent, startedRaw);
        const Json startedEnvelope =
            Json{{"method", "item/started"},
                 {"params", {{"threadId", "thread-user"}, {"turnId", "turn-user"}, {"item", startedRaw}, {"startedAtMs", 101}}}};
        const std::vector<backend::BackendEvent> startedEvents =
            reducer.translate(typed::Event{typed::ItemStarted{typed::ThreadItem{startedItem}, 101, startedEnvelope}});
        const auto* startedUpsert = startedEvents.size() == 1 ? std::get_if<backend::ItemUpserted>(&startedEvents.front()) : nullptr;
        result.expectTrue(startedUpsert && startedUpsert->threadId.value == "thread-user" && startedUpsert->turnId.value == "turn-user" &&
                              startedUpsert->lifecycle == backend::ItemLifecycle::Started && startedUpsert->occurredAtMs == 101,
                          "userMessage start translates to a canonical item upsert at its envelope location");
        if (startedUpsert) {
            reducer.apply(state, *startedUpsert);
        }

        const backend::ItemState* startedState = findItem(state, "thread-user", "turn-user", "user-item");
        const auto* canonicalStarted = startedState ? std::get_if<typed::UserMessageThreadItem>(&startedState->item) : nullptr;
        result.expectTrue(canonicalStarted && canonicalStarted->metadata.raw.at("content") == startedContent &&
                              !canonicalStarted->clientId && canonicalStarted->metadata.raw == startedRaw &&
                              startedState->lifecycle == backend::ItemLifecycle::Started && startedState->startedAtMs == 101 &&
                              !startedState->completedAtMs && state.recentExtensions.empty(),
                          "userMessage start retains complete content, nullable client ID, raw item, timestamp, and no extension fallback");

        const Json completedContent = Json::array({Json{{"type", "text"}, {"text", "Answer just with OK!"}},
                                                   Json{{"type", "futureContent"}, {"payload", Json::array({1, 2, 3})}}});
        const Json completedRaw = {
            {"type", "userMessage"}, {"id", "user-item"}, {"clientId", nullptr}, {"content", completedContent}, {"future", 2}};
        const typed::UserMessageThreadItem completedItem =
            userMessageItem("thread-user", "turn-user", "user-item", completedContent, completedRaw);
        const Json completedEnvelope =
            Json{{"method", "item/completed"},
                 {"params", {{"threadId", "thread-user"}, {"turnId", "turn-user"}, {"item", completedRaw}, {"completedAtMs", 202}}}};
        const std::vector<backend::BackendEvent> completedEvents =
            reducer.translate(typed::Event{typed::ItemCompleted{typed::ThreadItem{completedItem}, 202, completedEnvelope}});
        const auto* completedUpsert = completedEvents.size() == 1 ? std::get_if<backend::ItemUpserted>(&completedEvents.front()) : nullptr;
        result.expectTrue(completedUpsert && completedUpsert->threadId.value == "thread-user" &&
                              completedUpsert->turnId.value == "turn-user" &&
                              completedUpsert->lifecycle == backend::ItemLifecycle::Completed && completedUpsert->occurredAtMs == 202,
                          "userMessage completion translates to a canonical terminal item upsert");
        if (completedUpsert) {
            reducer.apply(state, *completedUpsert);
        }

        const backend::TurnState* userTurn = findTurn(state, "thread-user", "turn-user");
        const backend::ItemState* completedState = findItem(state, "thread-user", "turn-user", "user-item");
        const auto* canonicalCompleted = completedState ? std::get_if<typed::UserMessageThreadItem>(&completedState->item) : nullptr;
        result.expectTrue(
            userTurn && userTurn->items.size() == 1 && userTurn->itemOrder.size() == 1 &&
                userTurn->itemOrder.front().value == "user-item" && canonicalCompleted &&
                canonicalCompleted->metadata.raw.at("content") == completedContent && canonicalCompleted->metadata.raw == completedRaw &&
                completedState->lifecycle == backend::ItemLifecycle::Completed && completedState->startedAtMs == 101 &&
                completedState->completedAtMs == 202 && state.recentExtensions.empty(),
            "userMessage completion updates the same canonical item and retains both lifecycle timestamps without extensions");

        const backend::Snapshot itemSnapshot = backend::makeSnapshot(state);
        const Json& userMessageData = itemSnapshot.threads[0].turns[0].items[0].data;
        result.expectTrue(itemSnapshot.threads.size() == 1 && itemSnapshot.threads[0].turns.size() == 1 &&
                              itemSnapshot.threads[0].turns[0].items.size() == 1 &&
                              itemSnapshot.threads[0].turns[0].items[0].type == "user_message" &&
                              userMessageData.at("content").is_array() && userMessageData.at("content") == completedContent &&
                              userMessageData.at("clientId").is_null() && !userMessageData.at("contentTruncated").get<bool>() &&
                              userMessageData.at("originalContentBytes") == completedContent.dump().size() &&
                              userMessageData.at("retainedContentBytes") == completedContent.dump().size() &&
                              userMessageData.at("originalContentItems") == completedContent.size() &&
                              userMessageData.at("retainedContentItems") == completedContent.size(),
                          "small userMessage snapshots preserve array content and report equal original and retained bounds");

        typed::Turn terminalTurn = turn(
            "thread-user", "turn-user", typed::TurnStatus::completed(), std::vector<typed::ThreadItem>{typed::ThreadItem{completedItem}});
        reducer.apply(state, backend::TurnCompleted{std::move(terminalTurn)});
        completedState = findItem(state, "thread-user", "turn-user", "user-item");
        result.expectTrue(completedState && completedState->lifecycle == backend::ItemLifecycle::Completed &&
                              completedState->startedAtMs == 101 && completedState->completedAtMs == 202,
                          "a later terminal turn snapshot does not erase item lifecycle timestamps");
    }

    void testUserMessageSnapshotBounding(tests::support::TestResult& result) {
        backend::Reducer reducer;

        backend::BackendState textualState;
        typed::UserMessageThreadItem textualItem;
        textualItem.metadata = metadata("thread-text", "turn-text", "item-text");
        typed::TextInput firstText;
        firstText.text = "hello";
        typed::ImageUrlInput image;
        image.url = "https://private.invalid/image.png";
        typed::TextInput secondText;
        secondText.text = "Grüße 🌍";
        typed::SkillInput skill;
        skill.name = "private-skill";
        skill.path = "/sensitive/skill/path";
        textualItem.content = {firstText, image, secondText, skill};
        const Json textualContent = Json::array({Json{{"type", "text"}, {"text", firstText.text}},
                                                 Json{{"type", "image"}, {"url", image.url}},
                                                 Json{{"type", "text"}, {"text", secondText.text}},
                                                 Json{{"type", "skill"}, {"name", skill.name}, {"path", skill.path}}});
        textualItem.metadata.raw["content"] = textualContent;
        textualItem.clientId = typed::ClientUserMessageId{"client-text"};
        reducer.apply(textualState,
                      backend::ItemUpserted{typed::ThreadId{"thread-text"},
                                            typed::TurnId{"turn-text"},
                                            typed::ThreadItem{textualItem},
                                            backend::ItemLifecycle::Completed,
                                            5});
        const backend::Snapshot textualSnapshot = backend::makeSnapshot(textualState);
        const Json& textualData = textualSnapshot.threads[0].turns[0].items[0].data;
        const std::string expectedText = firstText.text + "\n\n" + secondText.text;
        result.expectTrue(textualData.at("content") == textualContent && textualData.at("text") == expectedText &&
                              !textualData.at("textTruncated").get<bool>() &&
                              textualData.at("originalTextBytes") == expectedText.size() &&
                              textualData.at("retainedTextBytes") == expectedText.size() && textualData.at("textFragments") == 2 &&
                              textualData.at("nonTextItems") == 2 && textualData.at("clientId") == "client-text",
                          "typed userMessage text is projected in order with an explicit separator, UTF-8, and non-text counts");

        backend::BackendState longTextState;
        typed::UserMessageThreadItem longTextItem;
        longTextItem.metadata = metadata("thread-long-text", "turn-long-text", "item-long-text");
        typed::TextInput longText;
        longText.text = std::string(backend::MaxProjectedUserMessageTextBytes - 1, 'x') + "€";
        longTextItem.content = {longText};
        const Json longTextContent = Json::array({Json{{"type", "text"}, {"text", longText.text}}});
        longTextItem.metadata.raw["content"] = longTextContent;
        reducer.apply(longTextState,
                      backend::ItemUpserted{typed::ThreadId{"thread-long-text"},
                                            typed::TurnId{"turn-long-text"},
                                            typed::ThreadItem{longTextItem},
                                            backend::ItemLifecycle::Completed,
                                            6});
        const backend::Snapshot longTextSnapshot = backend::makeSnapshot(longTextState);
        const Json& longTextData = longTextSnapshot.threads[0].turns[0].items[0].data;
        const std::string& retainedLongText = longTextData.at("text").get_ref<const std::string&>();
        result.expectTrue(longTextData.at("textTruncated").get<bool>() &&
                              longTextData.at("originalTextBytes") == longText.text.size() &&
                              longTextData.at("retainedTextBytes") == retainedLongText.size() &&
                              retainedLongText == std::string(backend::MaxProjectedUserMessageTextBytes - 1, 'x') &&
                              longTextData.at("contentTruncated").get<bool>() && longTextData.dump().size() <=
                                  backend::MaxSerializedUserMessageDataBytes,
                          "backend text and retained-content bounds report independent truthful truncation on a UTF-8 boundary");

        backend::BackendState smallState;
        const Json smallContent =
            Json::array({Json{{"type", "text"}, {"text", "small"}}, Json{{"type", "future"}, {"nested", Json{{"kept", true}}}}});
        typed::UserMessageThreadItem smallItem =
            userMessageItem("thread-small", "turn-small", "item-small", smallContent, Json{{"id", "item-small"}}, "client-small");
        reducer.apply(smallState,
                      backend::ItemUpserted{typed::ThreadId{"thread-small"},
                                            typed::TurnId{"turn-small"},
                                            typed::ThreadItem{smallItem},
                                            backend::ItemLifecycle::Completed,
                                            10});
        const backend::Snapshot smallStateSnapshot = backend::makeSnapshot(smallState);
        const backend::ItemSnapshot& smallSnapshot = smallStateSnapshot.threads[0].turns[0].items[0];
        const Json& smallData = smallSnapshot.data;
        result.expectTrue(
            smallData.at("clientId") == "client-small" && smallData.at("content").is_array() && smallData.at("content") == smallContent &&
                !smallData.at("contentTruncated").get<bool>() && smallData.at("originalContentBytes") == smallContent.dump().size() &&
                smallData.at("retainedContentBytes") == smallContent.dump().size() &&
                smallData.at("originalContentItems") == smallContent.size() && smallData.at("retainedContentItems") == smallContent.size(),
            "small userMessage content and a non-null clientId remain lossless with complete bound metadata");

        backend::BackendState largeState;
        Json largeContent = Json::array();
        largeContent.push_back(Json{{"type", "futureNested"}, {"payload", Json{{"unknown", Json::array({1, Json{{"deep", true}}})}}}});
        for (std::size_t index = 0; index < 8; ++index) {
            largeContent.push_back(Json{{"type", "futureChunk"},
                                        {"index", index},
                                        {"payload", std::string(12U * 1024U, static_cast<char>('a' + index))},
                                        {"opaque", Json{{"index", index}, {"enabled", index % 2 == 0}}}});
        }
        typed::UserMessageThreadItem largeItem =
            userMessageItem("thread-large", "turn-large", "item-large", largeContent, Json{{"id", "item-large"}}, "client-large");
        reducer.apply(largeState,
                      backend::ItemUpserted{typed::ThreadId{"thread-large"},
                                            typed::TurnId{"turn-large"},
                                            typed::ThreadItem{largeItem},
                                            backend::ItemLifecycle::Completed,
                                            20});
        const backend::Snapshot largeStateSnapshot = backend::makeSnapshot(largeState);
        const backend::ItemSnapshot& largeSnapshot = largeStateSnapshot.threads[0].turns[0].items[0];
        const Json& largeData = largeSnapshot.data;
        const Json& retainedContent = largeData.at("content");
        const std::size_t retainedItems = retainedContent.size();
        bool retainedPrefixUnchanged = retainedItems > 0 && retainedItems < largeContent.size();
        for (std::size_t index = 0; index < retainedItems; ++index) {
            retainedPrefixUnchanged = retainedPrefixUnchanged && retainedContent[index] == largeContent[index] &&
                                      retainedContent[index].dump() == largeContent[index].dump();
        }
        const backend::ItemState* canonicalLarge = findItem(largeState, "thread-large", "turn-large", "item-large");
        const auto* canonicalLargeUser = canonicalLarge ? std::get_if<typed::UserMessageThreadItem>(&canonicalLarge->item) : nullptr;
        result.expectTrue(largeContent.dump().size() > backend::MaxSerializedUserMessageDataBytes && retainedContent.is_array() &&
                              largeData.at("contentTruncated").get<bool>() && retainedPrefixUnchanged &&
                              largeData.at("originalContentItems") == largeContent.size() &&
                              largeData.at("retainedContentItems") == retainedItems &&
                              largeData.at("originalContentBytes") == largeContent.dump().size() &&
                              largeData.at("retainedContentBytes") == retainedContent.dump().size() &&
                              largeData.dump().size() <= backend::MaxSerializedUserMessageDataBytes && canonicalLargeUser &&
                              canonicalLargeUser->metadata.raw.at("content") == largeContent && !largeSnapshot.contentTruncated &&
                              largeSnapshot.droppedContentBytes == 0,
                          "large userMessage snapshots retain an unchanged ordered prefix of complete opaque entries within 64 KiB");

        backend::BackendState oversizedFirstState;
        const Json oversizedFirstContent = Json::array({Json{{"type", "futureHuge"}, {"payload", std::string(70U * 1024U, 'z')}}});
        typed::UserMessageThreadItem oversizedFirstItem =
            userMessageItem("thread-first", "turn-first", "item-first", oversizedFirstContent, Json{{"id", "item-first"}});
        reducer.apply(oversizedFirstState,
                      backend::ItemUpserted{typed::ThreadId{"thread-first"},
                                            typed::TurnId{"turn-first"},
                                            typed::ThreadItem{oversizedFirstItem},
                                            backend::ItemLifecycle::Completed,
                                            30});
        const backend::Snapshot oversizedFirstSnapshot = backend::makeSnapshot(oversizedFirstState);
        const Json& oversizedFirstData = oversizedFirstSnapshot.threads[0].turns[0].items[0].data;
        result.expectTrue(oversizedFirstData.at("content").is_array() && oversizedFirstData.at("content").empty() &&
                              oversizedFirstData.at("contentTruncated").get<bool>() && oversizedFirstData.at("originalContentItems") == 1 &&
                              oversizedFirstData.at("retainedContentItems") == 0 &&
                              oversizedFirstData.at("originalContentBytes") == oversizedFirstContent.dump().size() &&
                              oversizedFirstData.at("retainedContentBytes") == Json::array().dump().size() &&
                              oversizedFirstData.dump().size() <= backend::MaxSerializedUserMessageDataBytes,
                          "an oversized first userMessage entry yields an empty array without exposing partial JSON or text");
    }

    void testUnknownItemCommonMetadataFallbacks(tests::support::TestResult& result) {
        backend::Reducer reducer;
        backend::BackendState state;

        typed::UnknownItem located;
        located.type = "futureItem";
        located.raw = Json{{"type", "futureItem"}, {"id", "unknown-located"}, {"future", Json::array({1, 2, 3})}};
        located.diagnostic = typed::DecodeDiagnostic{typed::DecodeIssueKind::MalformedKnownPayload,
                                                     typed::DecodeIssueSeverity::ProtocolWarning,
                                                     "ThreadItem",
                                                     "$.future",
                                                     "known protocol payload did not match its typed contract"};
        located.metadata.id = typed::ItemId{"unknown-located"};
        located.metadata.threadId = typed::ThreadId{"thread-unknown"};
        located.metadata.turnId = typed::TurnId{"turn-unknown"};
        const Json locatedEnvelope =
            Json{{"method", "item/started"},
                 {"params", {{"threadId", "thread-unknown"}, {"turnId", "turn-unknown"}, {"item", located.raw}, {"startedAtMs", 303}}}};
        const std::vector<backend::BackendEvent> locatedEvents =
            reducer.translate(typed::Event{typed::ItemStarted{typed::ThreadItem{located}, 303, locatedEnvelope}});
        const auto* locatedUpsert = locatedEvents.size() == 1 ? std::get_if<backend::ItemUpserted>(&locatedEvents.front()) : nullptr;
        result.expectTrue(locatedUpsert && locatedUpsert->threadId.value == "thread-unknown" &&
                              locatedUpsert->turnId.value == "turn-unknown",
                          "an unknown item with valid common metadata translates canonically instead of reporting a false location error");
        if (locatedUpsert) {
            reducer.apply(state, *locatedUpsert);
        }
        const backend::ItemState* locatedState = findItem(state, "thread-unknown", "turn-unknown", "unknown-located");
        const auto* canonicalUnknown = locatedState ? std::get_if<typed::UnknownItem>(&locatedState->item) : nullptr;
        result.expectTrue(canonicalUnknown && canonicalUnknown->metadata.id && canonicalUnknown->metadata.id->value == "unknown-located" &&
                              canonicalUnknown->metadata.threadId && canonicalUnknown->metadata.threadId->value == "thread-unknown" &&
                              canonicalUnknown->metadata.turnId && canonicalUnknown->metadata.turnId->value == "turn-unknown" &&
                              canonicalUnknown->raw == located.raw && canonicalUnknown->diagnostic == located.diagnostic &&
                              state.recentExtensions.empty(),
                          "canonical unknown items retain their ID, envelope location, raw JSON, and structured diagnostic");

        backend::ReducerOptions boundedOptions;
        boundedOptions.retainedExtensions = 1;
        boundedOptions.maxExtensionBytes = 64;
        backend::Reducer boundedReducer(boundedOptions);
        backend::BackendState noIdState;
        typed::UnknownItem noId;
        noId.type = "futureWithoutId";
        noId.raw = Json{{"type", "futureWithoutId"}, {"payload", std::string(256, 'x')}};
        noId.metadata.threadId = typed::ThreadId{"thread-no-id"};
        noId.metadata.turnId = typed::TurnId{"turn-no-id"};
        const Json noIdEnvelope =
            Json{{"method", "item/started"},
                 {"params", {{"threadId", "thread-no-id"}, {"turnId", "turn-no-id"}, {"item", noId.raw}, {"startedAtMs", 404}}}};
        const std::vector<backend::BackendEvent> noIdEvents =
            boundedReducer.translate(typed::Event{typed::ItemStarted{typed::ThreadItem{noId}, 404, noIdEnvelope}});
        const auto* noIdUpsert = noIdEvents.size() == 1 ? std::get_if<backend::ItemUpserted>(&noIdEvents.front()) : nullptr;
        result.expectTrue(noIdUpsert && noIdUpsert->threadId.value == "thread-no-id" && noIdUpsert->turnId.value == "turn-no-id",
                          "an unknown item with a valid location but no stable ID reaches canonical ID validation");
        if (noIdUpsert) {
            boundedReducer.apply(noIdState, *noIdUpsert);
        }
        const backend::ExtensionRecord* noIdExtension =
            noIdState.recentExtensions.size() == 1 ? &noIdState.recentExtensions.front() : nullptr;
        result.expectTrue(noIdState.threads.empty() && noIdExtension && noIdExtension->method == "codex/item-without-id" &&
                              noIdExtension->decodingError == "typed item has no stable id" &&
                              noIdExtension->originalPayloadBytes.has_value() && noIdExtension->payload.value("truncated", false) &&
                              noIdExtension->payload.value("omitted", false),
                          "a truly ID-less item uses the bounded item-without-id extension fallback");

        backend::BackendState missingLocationState;
        typed::UnknownItem missingLocation;
        missingLocation.type = "futureMissingLocation";
        missingLocation.raw = Json{{"type", "futureMissingLocation"}, {"id", "unknown-missing-location"}};
        missingLocation.metadata.id = typed::ItemId{"unknown-missing-location"};
        missingLocation.metadata.threadId = typed::ThreadId{"thread-missing-location"};
        const Json missingLocationEnvelope =
            Json{{"method", "item/started"}, {"params", {{"threadId", "thread-missing-location"}, {"item", missingLocation.raw}}}};
        const std::vector<backend::BackendEvent> missingLocationEvents =
            reducer.translate(typed::Event{typed::ItemStarted{typed::ThreadItem{missingLocation}, 505, missingLocationEnvelope}});
        const auto* locationExtension =
            missingLocationEvents.size() == 1 ? std::get_if<backend::CodexExtensionReceived>(&missingLocationEvents.front()) : nullptr;
        result.expectTrue(locationExtension && locationExtension->method == "item/started" &&
                              locationExtension->payload == missingLocationEnvelope &&
                              locationExtension->decodingError == "item event omitted threadId or turnId",
                          "an item genuinely missing part of its envelope location retains the lifecycle extension fallback");
        if (locationExtension) {
            reducer.apply(missingLocationState, *locationExtension);
        }
        result.expectTrue(missingLocationState.threads.empty() && missingLocationState.recentExtensions.size() == 1,
                          "an unusable item location is not inserted into canonical thread state");
    }

    void testUnknownPreservationAndTranslation(tests::support::TestResult& result) {
        backend::ReducerOptions options;
        options.retainedExtensions = 2;
        backend::Reducer reducer(options);
        backend::BackendState state;

        typed::UnknownItem unknown;
        unknown.type = "futureItem";
        unknown.raw = Json{{"id", "unknown-item"}, {"future", Json::array({1, 2, 3})}};
        unknown.diagnostic = typed::DecodeDiagnostic{typed::DecodeIssueKind::UnknownDiscriminator,
                                                     typed::DecodeIssueSeverity::ForwardCompatibility,
                                                     "ThreadItem",
                                                     "$.type",
                                                     "unrecognized protocol discriminator was retained as raw JSON"};
        reducer.apply(state,
                      backend::ItemUpserted{typed::ThreadId{"thread-extension"},
                                            typed::TurnId{"turn-extension"},
                                            typed::ThreadItem{unknown},
                                            backend::ItemLifecycle::Started,
                                            std::nullopt});
        const backend::ItemState* unknownState = findItem(state, "thread-extension", "turn-extension", "unknown-item");
        result.expectTrue(unknownState && backend::itemType(unknownState->item) == "futureItem" &&
                              std::get<typed::UnknownItem>(unknownState->item).raw == unknown.raw,
                          "unknown typed items with stable IDs remain visible with their extension payload");

        const typed::DecodeDiagnostic futureDiagnostic{typed::DecodeIssueKind::UnknownMethod,
                                                       typed::DecodeIssueSeverity::ForwardCompatibility,
                                                       "future/event",
                                                       "$.method",
                                                       "unrecognized App Server notification method"};
        const typed::UnknownEvent unknownEvent{"future/event", Json{{"value", 7}}, Json{{"method", "future/event"}}, futureDiagnostic};
        const std::vector<backend::BackendEvent> translated = reducer.translate(typed::Event{unknownEvent});
        const auto* extension = translated.size() == 1 ? std::get_if<backend::CodexExtensionReceived>(&translated[0]) : nullptr;
        result.expectTrue(
            extension && extension->method == "future/event" && extension->payload == unknownEvent.params && !extension->decodingError &&
                extension->diagnostic && extension->diagnostic->kind == futureDiagnostic.kind &&
                extension->diagnostic->severity == futureDiagnostic.severity &&
                extension->diagnostic->surface == futureDiagnostic.surface &&
                extension->diagnostic->fieldPath == futureDiagnostic.fieldPath,
            "unknown typed Codex events translate to deliberately namespaced backend extensions with structured classification");

        const typed::DecodeDiagnostic malformedDiagnostic{typed::DecodeIssueKind::MalformedKnownPayload,
                                                          typed::DecodeIssueSeverity::ProtocolWarning,
                                                          "known/event",
                                                          "$.params.secret",
                                                          "known protocol payload did not match its typed contract"};
        const typed::UnknownEvent malformedEvent{
            "known/event", Json{{"secret", "must-not-appear"}}, Json{{"method", "known/event"}}, malformedDiagnostic};
        const std::vector<backend::BackendEvent> malformedTranslated = reducer.translate(typed::Event{malformedEvent});
        const auto* malformedExtension =
            malformedTranslated.size() == 1 ? std::get_if<backend::CodexExtensionReceived>(&malformedTranslated[0]) : nullptr;
        result.expectTrue(malformedExtension && malformedExtension->decodingError &&
                              *malformedExtension->decodingError ==
                                  "known protocol payload did not match its typed contract at $.params.secret" &&
                              malformedExtension->decodingError->find("must-not-appear") == std::string::npos,
                          "malformed-known typed diagnostics derive a value-free structural backend string");

        if (extension) {
            reducer.apply(state, *extension);
        }
        const backend::ExtensionRecord* retainedUnknown =
            state.recentExtensions.size() == 1 ? &state.recentExtensions.front() : nullptr;
        result.expectTrue(retainedUnknown && retainedUnknown->method == "future/event" &&
                              retainedUnknown->payload == unknownEvent.params && retainedUnknown->diagnostic &&
                              retainedUnknown->diagnostic->kind == futureDiagnostic.kind &&
                              retainedUnknown->diagnostic->surface == futureDiagnostic.surface &&
                              retainedUnknown->diagnostic->severity == typed::DecodeIssueSeverity::ForwardCompatibility,
                          "the bounded preservation state retains the raw future payload and forward-compatibility classification");

        struct SyntheticUnmodeledTypedEvent {
            std::string surface;
            Json raw;
            std::optional<std::string> decodingError;
            std::optional<typed::DecodeDiagnostic> diagnostic;
        };
        const SyntheticUnmodeledTypedEvent synthetic{
            "future/modeled-but-unreduced",
            Json{{"threadId", "thread-unmodeled"}, {"future", Json::array({"kept", 9})}},
            "known payload did not satisfy the typed shape",
            typed::DecodeDiagnostic{typed::DecodeIssueKind::MalformedKnownPayload,
                                    typed::DecodeIssueSeverity::ProtocolWarning,
                                    "future/modeled-but-unreduced",
                                    "$.params.future[1]",
                                    "expected a string"}};
        const backend::CodexExtensionReceived syntheticExtension = backend::detail::preserveUnmodeledTypedEvent(
            {synthetic.surface, synthetic.raw, synthetic.decodingError, synthetic.diagnostic});
        backend::BackendState syntheticState;
        reducer.apply(syntheticState, syntheticExtension);
        const backend::ExtensionRecord* retainedSynthetic =
            syntheticState.recentExtensions.size() == 1 ? &syntheticState.recentExtensions.front() : nullptr;
        result.expectTrue(
            syntheticExtension.method == synthetic.surface && syntheticExtension.payload == synthetic.raw &&
                syntheticExtension.diagnostic && synthetic.diagnostic &&
                syntheticExtension.diagnostic->kind == synthetic.diagnostic->kind &&
                syntheticExtension.diagnostic->severity == synthetic.diagnostic->severity &&
                syntheticExtension.diagnostic->fieldPath == synthetic.diagnostic->fieldPath && retainedSynthetic &&
                retainedSynthetic->method == synthetic.surface && retainedSynthetic->payload == synthetic.raw &&
                retainedSynthetic->diagnostic && retainedSynthetic->diagnostic->surface == synthetic.surface &&
                retainedSynthetic->diagnostic->kind == typed::DecodeIssueKind::MalformedKnownPayload &&
                retainedSynthetic->diagnostic->severity == typed::DecodeIssueSeverity::ProtocolWarning,
            "a private synthetic typed-but-unmodeled event uses the production preservation helper and retains malformed-known classification");
        result.expectTrue(syntheticState.provider.lifecycle == backend::ProviderLifecycle::Stopped && syntheticState.threads.empty() &&
                              syntheticState.threadOrder.empty() && syntheticState.pendingRequests.empty() &&
                              syntheticState.sessions.empty() && !syntheticState.controller && syntheticState.diagnostics.received == 0 &&
                              syntheticState.recentExtensions.size() == 1,
                          "typed-but-unmodeled preservation introduces no canonical domain state or reducer semantics");

        backend::ReducerOptions structuredBounds;
        structuredBounds.retainedExtensions = 1;
        structuredBounds.maxExtensionMethodBytes = 12;
        structuredBounds.maxExtensionBytes = 64;
        structuredBounds.maxExtensionDecodingErrorBytes = 16;
        backend::Reducer structuredReducer(structuredBounds);
        backend::BackendState structuredState;
        const std::string oversizedSyntheticPayload(256, 'r');
        const std::string diagnosticSurface(20, 's');
        const std::string diagnosticPath(24, 'p');
        const std::string diagnosticMessage(28, 'm');
        structuredReducer.apply(
            structuredState,
            backend::detail::preserveUnmodeledTypedEvent(
                {"future/bounded",
                 Json{{"kept", oversizedSyntheticPayload}},
                 std::nullopt,
                 typed::DecodeDiagnostic{typed::DecodeIssueKind::MalformedKnownPayload,
                                         typed::DecodeIssueSeverity::ProtocolWarning,
                                         diagnosticSurface,
                                         diagnosticPath,
                                         diagnosticMessage}}));
        const backend::ExtensionRecord* boundedStructured =
            structuredState.recentExtensions.size() == 1 ? &structuredState.recentExtensions.front() : nullptr;
        result.expectTrue(
            boundedStructured && boundedStructured->diagnostic && boundedStructured->diagnostic->surface.size() == 12 &&
                boundedStructured->diagnostic->fieldPath.size() == 16 && boundedStructured->diagnostic->message.size() == 16 &&
                boundedStructured->originalPayloadBytes.has_value() && boundedStructured->payload.value("truncated", false) &&
                boundedStructured->payload.value("omitted", false) &&
                boundedStructured->payload.dump().find(oversizedSyntheticPayload) == std::string::npos &&
                boundedStructured->originalDiagnosticBytes ==
                    diagnosticSurface.size() + diagnosticPath.size() + diagnosticMessage.size() &&
                boundedStructured->diagnostic->kind == typed::DecodeIssueKind::MalformedKnownPayload &&
                boundedStructured->diagnostic->severity == typed::DecodeIssueSeverity::ProtocolWarning,
            "structured compatibility diagnostics retain exact classification while every text field is bounded with original-size accounting");

        reducer.apply(state, backend::CodexExtensionReceived{"future/two", Json{{"value", 2}}, std::nullopt});
        reducer.apply(state, backend::CodexExtensionReceived{"future/three", Json{{"value", 3}}, std::nullopt});
        result.expectTrue(state.recentExtensions.size() == 2 && state.recentExtensions[0].method == "future/two" &&
                              state.recentExtensions[1].method == "future/three",
                          "unknown extension retention is bounded and evicts oldest records deterministically");

        backend::BackendState sensitiveState;
        const std::string accessToken = "extension-access-token-must-not-leak";
        const std::string secretAnswer = "extension-secret-answer-must-not-leak";
        const std::string longMethod(backend::MaxSnapshotExtensionMethodBytes + 17, 'm');
        const std::string longError(backend::MaxSnapshotExtensionDecodingErrorBytes + 19, 'e');
        reducer.apply(
            sensitiveState,
            backend::CodexExtensionReceived{longMethod,
                                            Json{{"safe", "visible"},
                                                 {"accessToken", accessToken},
                                                 {"Client_Secret", accessToken},
                                                 {"nested", {{"secret", true}, {"text", secretAnswer}, {"answer", secretAnswer}}}},
                                            longError});
        const backend::Snapshot sensitiveSnapshot = backend::makeSnapshot(sensitiveState);
        const backend::ExtensionSnapshot* safeExtension =
            sensitiveSnapshot.recentExtensions.size() == 1 ? &sensitiveSnapshot.recentExtensions.front() : nullptr;
        const std::string encodedSafeExtension = safeExtension ? safeExtension->payload.dump() : std::string();
        result.expectTrue(
            safeExtension && safeExtension->method.size() == backend::MaxSnapshotExtensionMethodBytes && safeExtension->methodTruncated &&
                safeExtension->decodingError && safeExtension->decodingError->size() == backend::MaxSnapshotExtensionDecodingErrorBytes &&
                safeExtension->decodingErrorTruncated && safeExtension->sensitiveFieldsRedacted &&
                safeExtension->payload.at("safe") == "visible" && encodedSafeExtension.find(accessToken) == std::string::npos &&
                encodedSafeExtension.find(secretAnswer) == std::string::npos,
            "public extension snapshots preserve safe fields while bounding text and recursively redacting credentials and secret answers");

        const std::string oversizedSecret(backend::MaxSnapshotExtensionPayloadBytes * 3, 's');
        reducer.apply(sensitiveState,
                      backend::CodexExtensionReceived{
                          "future/oversized", Json{{"accessToken", oversizedSecret}, {"safe", "would exceed the bound"}}, std::nullopt});
        const backend::Snapshot oversizedSnapshot = backend::makeSnapshot(sensitiveState);
        const backend::ExtensionSnapshot& oversizedExtension = oversizedSnapshot.recentExtensions.back();
        result.expectTrue(
            oversizedExtension.payloadTruncated && oversizedExtension.originalPayloadBytes.has_value() &&
                oversizedExtension.payload.dump().size() <= backend::MaxSnapshotExtensionPayloadBytes &&
                oversizedExtension.payload.value("omitted", false) &&
                oversizedExtension.payload.dump().find(oversizedSecret) == std::string::npos,
            "oversized extension payloads become explicit bounded omission records instead of leaking an unbatchable preview");

        backend::ReducerOptions manyOptions;
        manyOptions.retainedExtensions = backend::MaxSnapshotCodexExtensions + 2;
        backend::Reducer manyReducer(manyOptions);
        backend::BackendState manyState;
        for (std::size_t index = 0; index < backend::MaxSnapshotCodexExtensions + 2; ++index) {
            manyReducer.apply(manyState,
                              backend::CodexExtensionReceived{"future/" + std::to_string(index), Json{{"index", index}}, std::nullopt});
        }
        const backend::Snapshot manySnapshot = backend::makeSnapshot(manyState);
        result.expectTrue(
            manySnapshot.recentExtensions.size() == backend::MaxSnapshotCodexExtensions && manySnapshot.omittedRecentExtensions == 2 &&
                manySnapshot.recentExtensions.front().method == "future/2" && manySnapshot.recentExtensions.back().method == "future/65",
            "public snapshots retain a deterministic bounded newest suffix when a custom reducer keeps more extension records");

        const typed::AgentMessageDelta delta{
            typed::ThreadId{"thread-extension"}, typed::TurnId{"turn-extension"}, typed::ItemId{"unknown-item"}, "x", Json::object()};
        const std::vector<backend::BackendEvent> translatedDelta = reducer.translate(typed::Event{delta});
        const auto* content = translatedDelta.size() == 1 ? std::get_if<backend::ItemContentChanged>(&translatedDelta[0]) : nullptr;
        result.expectTrue(content && content->kind == backend::ItemContentChanged::Kind::AgentText && content->delta == "x" &&
                              !std::holds_alternative<backend::CodexExtensionReceived>(translatedDelta[0]),
                          "known typed deltas normalize to backend content changes instead of raw envelopes");
    }

    typed::CommandApprovalRequest approvalRequest() {
        return typed::CommandApprovalRequest{
            .requestId = ServerRequestId{std::string("server-request")},
            .requestToken = ServerRequestToken{77},
            .threadId = typed::ThreadId{"thread-request"},
            .turnId = typed::TurnId{"turn-request"},
            .itemId = typed::ItemId{"item-request"},
            .startedAtMs = 13,
            .command = std::string("make test"),
            .cwd = std::string("/tmp/project"),
            .reason = std::string("needs approval"),
            .details = Json{{"future", true}},
            .raw = Json{{"privateOccurrence", 77}},
            .canonicalParams = {},
            .diagnostics = {},
        };
    }

    void testPendingRequestsAndSessions(tests::support::TestResult& result) {
        backend::BackendState state;
        backend::Reducer reducer;
        const backend::PendingRequestState pending{backend::PendingRequestId{9}, typed::TypedServerRequest{approvalRequest()}, 3};

        const backend::Reduction added = reducer.apply(state, backend::PendingRequestAdded{pending});
        result.expectTrue(added.flushImmediately && state.pendingRequests.size() == 1 &&
                              state.pendingRequests.at(backend::PendingRequestId{9}).connectionGeneration == 3,
                          "pending request insertion retains the exact typed occurrence until an authoritative removal");

        const backend::Snapshot pendingSnapshot = backend::makeSnapshot(state);
        result.expectTrue(pendingSnapshot.pendingRequests.size() == 1 && pendingSnapshot.pendingRequests[0].id.value() == 9 &&
                              pendingSnapshot.pendingRequests[0].type == "command_approval" &&
                              pendingSnapshot.pendingRequests[0].details.value("commandRedacted", false) &&
                              pendingSnapshot.pendingRequests[0].details.value("commandBytes", 0) == 9 &&
                              pendingSnapshot.pendingRequests[0].details.value("cwdRedacted", false) &&
                              pendingSnapshot.pendingRequests[0].details.value("reasonRedacted", false) &&
                              pendingSnapshot.pendingRequests[0].details.dump().find("make test") == std::string::npos &&
                              pendingSnapshot.pendingRequests[0].details.dump().find("/tmp/project") == std::string::npos &&
                              pendingSnapshot.pendingRequests[0].details.dump().find("needs approval") == std::string::npos &&
                              !pendingSnapshot.pendingRequests[0].details.contains("privateOccurrence"),
                          "pending request snapshot exposes only redacted command/path/reason metadata without occurrence tokens");

        const std::string unknownAccessToken = "unknown-request-access-token-must-not-leak";
        const std::string unknownSecretAnswer = "unknown-request-secret-answer-must-not-leak";
        const std::string occurrenceSentinel = "unknown-request-occurrence-token-must-not-leak";
        const std::string unknownDecodingError(backend::MaxSnapshotExtensionDecodingErrorBytes + 23, 'd');
        const typed::UnknownServerRequest unknownRequest{
            ServerRequestId{std::string("unknown-request")},
            ServerRequestToken{88},
            "future/request",
            Json{{"safe", "visible"}, {"accessToken", unknownAccessToken}, {"question", {{"secret", true}, {"text", unknownSecretAnswer}}}},
            Json{{"occurrenceToken", occurrenceSentinel}},
            typed::DecodeDiagnostic{typed::DecodeIssueKind::MalformedKnownPayload,
                                    typed::DecodeIssueSeverity::ProtocolWarning,
                                    "future/request",
                                    "",
                                    unknownDecodingError}};
        reducer.apply(state,
                      backend::PendingRequestAdded{
                          backend::PendingRequestState{backend::PendingRequestId{10}, typed::TypedServerRequest{unknownRequest}, 3}});
        const backend::Snapshot unknownPendingSnapshot = backend::makeSnapshot(state);
        const auto unknownPending = std::find_if(unknownPendingSnapshot.pendingRequests.begin(),
                                                 unknownPendingSnapshot.pendingRequests.end(),
                                                 [](const backend::PendingRequestSnapshot& value) {
                                                     return value.id == backend::PendingRequestId{10};
                                                 });
        const std::string compactUnknownPending =
            unknownPending != unknownPendingSnapshot.pendingRequests.end() ? unknownPending->details.dump() : std::string();
        result.expectTrue(unknownPending != unknownPendingSnapshot.pendingRequests.end() && unknownPending->type == "unknown" &&
                              unknownPending->details["params"].value("safe", "") == "visible" &&
                              unknownPending->details.value("sensitiveFieldsRedacted", false) &&
                              unknownPending->details.value("decodingError", "").size() ==
                                  backend::MaxSnapshotExtensionDecodingErrorBytes &&
                              unknownPending->details.value("decodingErrorTruncated", false) &&
                              compactUnknownPending.find(unknownAccessToken) == std::string::npos &&
                              compactUnknownPending.find(unknownSecretAnswer) == std::string::npos &&
                              compactUnknownPending.find(occurrenceSentinel) == std::string::npos,
                          "compact unknown-request snapshots recursively redact secret params and never expose the raw occurrence token");

        result.expectTrue(state.pendingRequests.size() == 2,
                          "a pending request remains present when no successful response-removal transition occurs");
        const backend::Reduction removed =
            reducer.apply(state, backend::PendingRequestRemoved{backend::PendingRequestId{9}, "response enqueued"});
        result.expectTrue(removed.changed && removed.flushImmediately && state.pendingRequests.size() == 1 &&
                              state.pendingRequests.contains(backend::PendingRequestId{10}),
                          "successful response removal erases only that pending request and flushes interactively");

        const backend::SessionId observer{1};
        const backend::SessionId controller{2};
        reducer.apply(state, backend::SessionChanged{observer, true, backend::SessionRole::Observer});
        reducer.apply(state, backend::SessionChanged{controller, true, backend::SessionRole::Observer});
        result.expectTrue(state.sessions.size() == 2 && !state.controller,
                          "newly connected sessions are retained as observers without implicit controller assignment");

        reducer.apply(state, backend::ControllerChanged{controller});
        result.expectTrue(state.controller == controller && state.sessions.at(controller).role == backend::SessionRole::Controller &&
                              state.sessions.at(observer).role == backend::SessionRole::Observer,
                          "controller acquisition changes exactly one session role");

        reducer.apply(state, backend::ControllerChanged{std::nullopt});
        result.expectTrue(!state.controller && state.sessions.at(controller).role == backend::SessionRole::Observer,
                          "controller release returns the former controller to observer role");

        reducer.apply(state, backend::ControllerChanged{controller});
        reducer.apply(state, backend::SessionChanged{controller, false, backend::SessionRole::Observer});
        result.expectTrue(!state.controller && state.sessions.size() == 1 && state.sessions.contains(observer),
                          "controller disconnect removes only that session and releases ownership");
    }

    void testSnapshotDeterminism(tests::support::TestResult& result) {
        backend::BackendState state;
        backend::Reducer reducer;
        reducer.apply(state, backend::ThreadUpserted{thread("thread-z"), backend::EntityLoad::Summary});
        reducer.apply(state, backend::ThreadUpserted{thread("thread-a"), backend::EntityLoad::Summary});
        reducer.apply(state, backend::TurnUpserted{turn("thread-z", "turn-z")});
        reducer.apply(state, backend::TurnUpserted{turn("thread-z", "turn-a")});
        reducer.apply(state,
                      backend::ItemUpserted{typed::ThreadId{"thread-z"},
                                            typed::TurnId{"turn-z"},
                                            agentItem("thread-z", "turn-z", "item-z", "z"),
                                            backend::ItemLifecycle::Started,
                                            std::nullopt});
        reducer.apply(state,
                      backend::ItemUpserted{typed::ThreadId{"thread-z"},
                                            typed::TurnId{"turn-z"},
                                            agentItem("thread-z", "turn-z", "item-a", "a"),
                                            backend::ItemLifecycle::Started,
                                            std::nullopt});

        state.sequence = backend::SequenceNumber{42};
        const backend::Snapshot first = backend::makeSnapshot(state);
        const backend::Snapshot second = backend::makeSnapshot(state);
        result.expectTrue(first == second && !(first != second), "two snapshots of unchanged state compare equal");
        result.expectTrue(first.sequence.value() == 42 && first.threads[0].id == "thread-z" && first.threads[1].id == "thread-a" &&
                              first.threads[0].turns[0].id == "turn-z" && first.threads[0].turns[1].id == "turn-a" &&
                              first.threads[0].turns[0].items[0].id == "item-z" && first.threads[0].turns[0].items[1].id == "item-a",
                          "snapshot includes sequence and deterministic first-seen entity ordering");

        reducer.apply(state, backend::DiagnosticReceived{"changed"});
        result.expectTrue(first != backend::makeSnapshot(state), "a visible state transition changes snapshot equality");
    }

    void testA16bNotificationItemAndCapacityClosure(tests::support::TestResult& result) {
        static_assert(std::variant_size_v<typed::CanonicalServerNotification> == 68);
        static_assert(std::variant_size_v<typed::Event> == 69);

        backend::Reducer reducer;
        const std::span<const ai::openai::codex::detail::ServerNotificationCodecDescriptor> notificationDescriptors =
            ai::openai::codex::detail::serverNotificationCodecDescriptors();
        std::set<ai::openai::codex::detail::ServerNotificationTarget> notificationTargets;
        std::set<std::string_view> notificationMethods;
        bool exactStableNotificationRegistry = true;
        for (const ai::openai::codex::detail::ServerNotificationCodecDescriptor& descriptor : notificationDescriptors) {
            const ai::openai::codex::detail::ProtocolSurfaceEntry& entry = ai::openai::codex::detail::entryFor(descriptor.target);
            exactStableNotificationRegistry = exactStableNotificationRegistry && entry.key == descriptor.key &&
                                              entry.key.category == ai::openai::codex::detail::SurfaceCategory::ServerNotification &&
                                              entry.stability == ai::openai::codex::detail::Stability::Stable &&
                                              entry.backendCore == ai::openai::codex::detail::LayerStatus::Implemented &&
                                              entry.canonicalState == ai::openai::codex::detail::LayerStatus::Implemented &&
                                              notificationTargets.insert(descriptor.target).second &&
                                              notificationMethods.insert(descriptor.key.name).second;
        }
        result.expectTrue(notificationDescriptors.size() == 68 && notificationTargets.size() == 68 && notificationMethods.size() == 68 &&
                              notificationMethods.contains("error") && exactStableNotificationRegistry,
                          "the registry-derived stable notification set contains exactly 68 unique implemented backend/state identities");
        result.expectTrue(allEventAlternativesTranslate(reducer, std::make_index_sequence<std::variant_size_v<typed::Event>>{}),
                          "every stable typed notification projection and the forward-compatible unknown event translate nonempty");
        const std::vector<backend::BackendEvent> translatedError = reducer.translate(notificationEvent<typed::TurnErrorEvent>());
        result.expectTrue(translatedError.size() == 1 && std::holds_alternative<backend::TurnErrorUpdated>(translatedError.front()),
                          "the stable error notification has exactly one backend translation on the existing TurnErrorUpdated path");
        const std::vector<typed::Event> formerlyDropped{
            notificationEvent<typed::McpServerOauthLoginCompletedNotification>(),
            notificationEvent<typed::McpServerStatusUpdatedNotification>(),
            notificationEvent<typed::DeprecationNoticeNotification>(),
            notificationEvent<typed::ProcessExitedNotification>(),
            notificationEvent<typed::ProcessOutputDeltaNotification>(),
            notificationEvent<typed::RemoteControlStatusChangedNotification>(),
            notificationEvent<typed::ServerRequestResolvedNotification>(),
            notificationEvent<typed::WarningNotification>(),
            notificationEvent<typed::WindowsWorldWritableWarningNotification>(),
            notificationEvent<typed::WindowsSandboxSetupCompletedNotification>(),
        };
        const bool allFormerlyDroppedTranslate = std::ranges::all_of(formerlyDropped, [&reducer](const typed::Event& event) {
            return !reducer.translate(event).empty();
        });
        result.expectTrue(allFormerlyDroppedTranslate && formerlyDropped.size() == 10,
                          "all ten formerly empty stable notification translations now produce a backend event");

        backend::BackendState semanticState;
        semanticState.provider.generation = 1;
        reducer.apply(
            semanticState,
            backend::ThreadUpserted{thread("semantic-thread", {turn("semantic-thread", "semantic-turn")}), backend::EntityLoad::Full});
        reducer.apply(semanticState, backend::ProviderConnectionInvalidated{1, "test disconnect"});
        semanticState.provider.generation = 2;
        typed::ModelSafetyBufferingUpdatedNotification safety;
        safety.threadId = typed::ThreadId{"semantic-thread"};
        safety.turnId = typed::TurnId{"semantic-turn"};
        safety.model = typed::ModelId{"model-a"};
        safety.raw = Json{{"params", Json::object()}};
        reducer.apply(semanticState, reducer.translate(typed::Event{std::move(safety)}).front());
        const backend::TurnState* confirmedTurn = findTurn(semanticState, "semantic-thread", "semantic-turn");
        result.expectTrue(confirmedTurn && confirmedTurn->stamp == backend::SourceStamp{2, backend::Freshness::Current} &&
                              semanticState.threads.at("semantic-thread").stamp == backend::SourceStamp{1, backend::Freshness::Stale},
                          "an authoritative model/turn notification confirms only the turn and does not promote its stale parent thread");

        typed::ModelVerificationNotification verification;
        verification.threadId = typed::ThreadId{"semantic-thread"};
        verification.turnId = typed::TurnId{"semantic-turn"};
        for (std::size_t index = 0; index < 300; ++index) {
            typed::ModelVerification entry;
            entry.value = "verification-" + std::to_string(index);
            verification.verifications.push_back(std::move(entry));
        }
        verification.raw = Json{{"params", Json::object()}};
        reducer.apply(semanticState, reducer.translate(typed::Event{std::move(verification)}).front());
        const Json& retainedVerifications =
            semanticState.threads.at("semantic-thread").turns.at("semantic-turn").extensions.at("modelVerifications");
        result.expectTrue(retainedVerifications.at("entries").size() == 256 && retainedVerifications.at("total") == 300 &&
                              retainedVerifications.at("truncated") == true,
                          "model verification state is bounded and records its truncation without retaining an unbounded vector");

        typed::ThreadNameUpdatedNotification renamed;
        renamed.threadId = typed::ThreadId{"semantic-thread"};
        renamed.threadName = std::string{"renamed by provider"};
        renamed.raw = Json{{"params", Json::object()}};
        reducer.apply(semanticState, reducer.translate(typed::Event{std::move(renamed)}).front());
        result.expectTrue(semanticState.threads.at("semantic-thread").thread.title == "renamed by provider" &&
                              semanticState.threads.at("semantic-thread").stamp == backend::SourceStamp{2, backend::Freshness::Current},
                          "thread-level notifications update the typed thread aggregate and confirm that entity at the current generation");

        backend::BackendState domainState;
        domainState.provider.generation = 5;
        const auto applyDomainNotification = [&reducer, &domainState](typed::Event event) {
            const std::vector<backend::BackendEvent> translated = reducer.translate(event);
            return reducer.apply(domainState, translated.front());
        };
        typed::AccountLoginCompletedNotification loginCompleted;
        loginCompleted.loginId = typed::LoginId{"login-5"};
        loginCompleted.success = true;
        loginCompleted.raw = Json{{"params", Json::object()}};
        applyDomainNotification(typed::Event{std::move(loginCompleted)});
        typed::AccountRateLimitsUpdatedNotification rateLimitsUpdated;
        typed::RateLimitWindow primaryLimit;
        primaryLimit.usedPercent = 37;
        primaryLimit.resetsAt = std::int64_t{1234};
        rateLimitsUpdated.rateLimits.primary = std::move(primaryLimit);
        rateLimitsUpdated.rateLimits.planType = typed::PlanType::plus();
        rateLimitsUpdated.raw = Json{{"params", Json::object()}};
        applyDomainNotification(typed::Event{std::move(rateLimitsUpdated)});
        typed::AccountUpdatedNotification accountUpdated;
        accountUpdated.authMode = typed::AuthMode::chatgpt();
        accountUpdated.planType = typed::PlanType::plus();
        accountUpdated.raw = Json{{"params", Json::object()}};
        applyDomainNotification(typed::Event{std::move(accountUpdated)});
        typed::AppListUpdatedNotification appsUpdated;
        typed::AppInfo app;
        app.id = "app-5";
        app.name = "Application Five";
        app.isAccessible = true;
        app.isEnabled = false;
        appsUpdated.data.push_back(std::move(app));
        appsUpdated.raw = Json{{"params", Json::object()}};
        applyDomainNotification(typed::Event{std::move(appsUpdated)});
        typed::McpServerStatusUpdatedNotification mcpStartup;
        mcpStartup.name = "server-5";
        mcpStartup.status = typed::McpServerStartupState::ready();
        mcpStartup.raw = Json{{"params", Json::object()}};
        applyDomainNotification(typed::Event{std::move(mcpStartup)});
        typed::RemoteControlStatusChangedNotification remoteControl;
        remoteControl.status = typed::RemoteControlConnectionStatus::connected();
        remoteControl.environmentId = std::string{"environment-5"};
        remoteControl.installationId = "installation-5";
        remoteControl.serverName = "remote-5";
        remoteControl.raw = Json{{"params", Json::object()}};
        applyDomainNotification(typed::Event{std::move(remoteControl)});
        typed::WindowsSandboxSetupCompletedNotification windowsCompleted;
        windowsCompleted.mode = typed::WindowsSandboxSetupMode::elevated();
        windowsCompleted.success = true;
        windowsCompleted.raw = Json{{"params", Json::object()}};
        applyDomainNotification(typed::Event{std::move(windowsCompleted)});
        typed::ExternalAgentConfigImportProgressNotification importProgress;
        importProgress.importId = "shared-activity-id";
        importProgress.raw = Json{{"params", Json::object()}};
        applyDomainNotification(typed::Event{std::move(importProgress)});
        typed::McpServerOauthLoginCompletedNotification oauthCompleted;
        oauthCompleted.name = "shared-activity-id";
        oauthCompleted.success = true;
        oauthCompleted.raw = Json{{"params", Json::object()}};
        applyDomainNotification(typed::Event{std::move(oauthCompleted)});
        const backend::Snapshot domainSnapshot = backend::makeSnapshot(domainState);
        result.expectTrue(domainSnapshot.accounts.login && domainSnapshot.accounts.login->lifecycle == "completed" &&
                              domainSnapshot.accounts.rateLimits && domainSnapshot.accounts.rateLimits->primaryUsedPercent == 37 &&
                              domainSnapshot.accounts.authentication && domainSnapshot.accounts.authentication->authMode == "chatgpt" &&
                              domainSnapshot.integrations.apps && domainSnapshot.integrations.apps->entries.size() == 1 &&
                              domainSnapshot.integrations.apps->entries.front().id == "app-5",
                          "account and app notifications project bounded meaningful canonical and safe snapshot state");
        result.expectTrue(domainSnapshot.mcp.startup && domainSnapshot.mcp.startup->serverName == "server-5" &&
                              domainSnapshot.mcp.startup->status == "ready" && domainSnapshot.platform.remoteControl &&
                              domainSnapshot.platform.remoteControl->environmentId == "environment-5" &&
                              domainSnapshot.platform.windowsSandbox && domainSnapshot.platform.windowsSandbox->success == true,
                          "MCP, remote-control, and Windows notifications project bounded provider domain state");
        const bool activityKindsDoNotCollide =
            std::ranges::count_if(domainSnapshot.activities, [](const backend::ActivitySnapshot& activity) {
                return activity.subjectId == "shared-activity-id" &&
                       (activity.kind == "external_agent_import" || activity.kind == "mcp_oauth");
            }) == 2;
        result.expectTrue(
            activityKindsDoNotCollide && domainSnapshot.mcp.oauth && domainSnapshot.mcp.oauth->success == true,
            "activity identities include their typed kind and preserve meaningful bounded OAuth/import state without collisions");

        backend::BackendState operationState;
        operationState.provider.generation = 7;
        reducer.apply(operationState, backend::ThreadUpserted{thread("operation-thread"), backend::EntityLoad::Full});
        reducer.apply(operationState,
                      backend::ProviderOperationCompleted{"thread/name/set",
                                                          backend::BackendCommand{backend::ThreadSetName{typed::ThreadSetNameParams{
                                                              typed::ThreadId{"operation-thread"}, "operation name"}}},
                                                          backend::ProviderOperationValue{typed::Unit{}},
                                                          std::nullopt});
        const bool nameProjected = operationState.threads.at("operation-thread").thread.title == "operation name" &&
                                   operationState.threads.at("operation-thread").stamp.freshness == backend::Freshness::Current;
        reducer.apply(operationState,
                      backend::ProviderOperationCompleted{"thread/inject_items",
                                                          backend::BackendCommand{backend::ThreadInjectItems{typed::ThreadInjectItemsParams{
                                                              typed::ThreadId{"operation-thread"}, {Json::object()}}}},
                                                          backend::ProviderOperationValue{typed::Unit{}},
                                                          std::nullopt});
        const bool injectionStaledAggregate = operationState.threads.at("operation-thread").stamp.freshness == backend::Freshness::Stale;
        operationState.providerOperations.emplace(
            "mcpServerStatus/list",
            backend::ProviderOperationState{"mcpServerStatus/list",
                                            backend::ProviderOperationValue{typed::ListMcpServerStatusResponse{}}.index(),
                                            {7, backend::Freshness::Current}});
        backend::ProviderResultSummaryState mcpSummary;
        mcpSummary.method = "mcpServerStatus/list";
        mcpSummary.stamp = {7, backend::Freshness::Current};
        operationState.mcp.latestResults.emplace("mcpServerStatus/list", std::move(mcpSummary));
        reducer.apply(operationState,
                      backend::ProviderOperationCompleted{"config/mcpServer/reload",
                                                          backend::BackendCommand{backend::ConfigMcpServerReload{}},
                                                          backend::ProviderOperationValue{typed::Unit{}},
                                                          std::nullopt});
        const bool reloadStaledMcp =
            operationState.providerOperations.at("mcpServerStatus/list").stamp.freshness == backend::Freshness::Stale &&
            operationState.mcp.latestResults.at("mcpServerStatus/list").stamp.freshness == backend::Freshness::Stale;
        reducer.apply(operationState,
                      backend::ProviderOperationCompleted{
                          "thread/delete",
                          backend::BackendCommand{backend::ThreadDelete{typed::ThreadDeleteParams{typed::ThreadId{"operation-thread"}}}},
                          backend::ProviderOperationValue{typed::Unit{}},
                          std::nullopt});
        result.expectTrue(nameProjected && injectionStaledAggregate && reloadStaledMcp && operationState.threads.empty() &&
                              operationState.capacity.retainedThreads == 0,
                          "operation projections retain bounded semantic state, apply authoritative fields, stale incomplete caches, and "
                          "remove deleted subtrees");

        typed::AppsListResponse appListResult;
        appListResult.raw = Json{{"secret", std::string(128, 's')}};
        for (std::size_t index = 0; index < 300; ++index) {
            typed::AppInfo listed;
            listed.id = "app-" + std::to_string(index);
            listed.name = "listed application";
            listed.raw = Json{{"unbounded", std::string(128, 'r')}};
            appListResult.data.push_back(std::move(listed));
        }
        reducer.apply(operationState,
                      backend::ProviderOperationCompleted{"app/list",
                                                          backend::BackendCommand{backend::AppsList{typed::AppsListParams{}}},
                                                          backend::ProviderOperationValue{std::move(appListResult)},
                                                          std::nullopt});
        const auto& retainedApps = *operationState.integrations.appList;
        const backend::Snapshot operationSnapshot = backend::makeSnapshot(operationState);
        result.expectTrue(
            retainedApps.originalEntries == 300 && retainedApps.truncated && retainedApps.value.data.size() == 256 &&
                retainedApps.value.raw.empty() && retainedApps.value.data.front().raw.empty() &&
                operationState.providerOperations.at("app/list").resultAlternative ==
                    backend::ProviderOperationValue{typed::AppsListResponse{}}.index() &&
                operationState.integrations.latestResults.at("app/list").itemCount == 300 && operationSnapshot.integrations.apps &&
                operationSnapshot.integrations.apps->truncated,
            "stateful operation completion retains one bounded typed replacement plus semantic snapshot state, not raw result graphs");

        const auto completeOperation = [&reducer, &operationState](
                                           std::string method, backend::BackendCommand command, backend::ProviderOperationValue value) {
            return reducer.apply(
                operationState, backend::ProviderOperationCompleted{std::move(method), std::move(command), std::move(value), std::nullopt});
        };
        operationState.configuration.configuration = backend::ReplacementCache<typed::ConfigReadResponse>{
            typed::ConfigReadResponse{}, {}, {}, 0, false, {7, backend::Freshness::Current}};
        const std::size_t canonicalReplacementStringLimit = backend::ReducerOptions{}.maxNoticeDetailsBytes;
        typed::ConfigWriteResponse configWrite;
        configWrite.filePath = typed::AbsolutePath{std::string(canonicalReplacementStringLimit * 2, 'c')};
        configWrite.status = typed::WriteStatus::okOverridden();
        configWrite.version = std::string(2048, 'v');
        typed::OverriddenMetadata overridden;
        overridden.effectiveValue = Json{{"secret", std::string(2048, 'x')}};
        overridden.message = std::string(2048, 'm');
        configWrite.overriddenMetadata = std::move(overridden);
        completeOperation("config/batchWrite",
                          backend::BackendCommand{backend::ConfigBatchWrite{typed::ConfigBatchWriteParams{}}},
                          backend::ProviderOperationValue{std::move(configWrite)});

        typed::ExperimentalFeatureEnablementSetResponse enablement;
        for (std::size_t index = 0; index < 300; ++index) {
            enablement.enablement.emplace(typed::ExperimentalFeatureId{"feature-" + std::to_string(index)}, index % 2 == 0);
        }
        completeOperation(
            "experimentalFeature/enablement/set",
            backend::BackendCommand{backend::ExperimentalFeatureEnablementSet{typed::ExperimentalFeatureEnablementSetParams{}}},
            backend::ProviderOperationValue{std::move(enablement)});

        typed::MarketplaceAddResponse marketplaceAdd;
        marketplaceAdd.alreadyAdded = true;
        marketplaceAdd.installedRoot = typed::AbsolutePath{std::string(2048, 'r')};
        marketplaceAdd.marketplaceName = std::string(2048, 'm');
        completeOperation("marketplace/add",
                          backend::BackendCommand{backend::MarketplaceAdd{typed::MarketplaceAddParams{}}},
                          backend::ProviderOperationValue{std::move(marketplaceAdd)});
        typed::MarketplaceRemoveResponse marketplaceRemove;
        marketplaceRemove.installedRoot = typed::AbsolutePath{std::string(2048, 'd')};
        marketplaceRemove.marketplaceName = "removed-marketplace";
        completeOperation("marketplace/remove",
                          backend::BackendCommand{backend::MarketplaceRemove{typed::MarketplaceRemoveParams{}}},
                          backend::ProviderOperationValue{std::move(marketplaceRemove)});
        typed::MarketplaceUpgradeResponse marketplaceUpgrade;
        marketplaceUpgrade.errors.resize(300);
        marketplaceUpgrade.selectedMarketplaces.assign(300, std::string(2048, 's'));
        marketplaceUpgrade.upgradedRoots.assign(300, typed::AbsolutePath{std::string(2048, 'u')});
        completeOperation("marketplace/upgrade",
                          backend::BackendCommand{backend::MarketplaceUpgrade{typed::MarketplaceUpgradeParams{}}},
                          backend::ProviderOperationValue{std::move(marketplaceUpgrade)});

        typed::PluginInstallResponse pluginInstall;
        pluginInstall.authPolicy = typed::PluginAuthPolicy::onInstall();
        completeOperation("plugin/install",
                          backend::BackendCommand{backend::PluginInstall{typed::PluginInstallParams{}}},
                          backend::ProviderOperationValue{std::move(pluginInstall)});
        typed::PluginShareCheckoutResponse shareCheckout;
        shareCheckout.marketplaceName = "marketplace";
        shareCheckout.marketplacePath = typed::AbsolutePath{std::string(2048, 'a')};
        shareCheckout.pluginId = "plugin";
        shareCheckout.pluginName = "Plugin";
        shareCheckout.pluginPath = typed::AbsolutePath{std::string(2048, 'b')};
        shareCheckout.remotePluginId = "remote-plugin";
        shareCheckout.remoteVersion = std::string(2048, 'q');
        completeOperation("plugin/share/checkout",
                          backend::BackendCommand{backend::PluginShareCheckout{typed::PluginShareCheckoutParams{}}},
                          backend::ProviderOperationValue{std::move(shareCheckout)});
        typed::PluginShareSaveResponse shareSave;
        shareSave.remotePluginId = "remote-plugin";
        shareSave.shareUrl = std::string(2048, 'z');
        completeOperation("plugin/share/save",
                          backend::BackendCommand{backend::PluginShareSave{typed::PluginShareSaveParams{}}},
                          backend::ProviderOperationValue{std::move(shareSave)});
        typed::PluginShareUpdateTargetsResponse shareTargets;
        shareTargets.discoverability = typed::PluginShareDiscoverability::privateVisibility();
        shareTargets.principals.resize(300);
        completeOperation("plugin/share/updateTargets",
                          backend::BackendCommand{backend::PluginShareUpdateTargets{typed::PluginShareUpdateTargetsParams{}}},
                          backend::ProviderOperationValue{std::move(shareTargets)});
        typed::SkillsConfigWriteResponse skillsWrite;
        skillsWrite.effectiveEnabled = true;
        completeOperation("skills/config/write",
                          backend::BackendCommand{backend::SkillsConfigWrite{typed::SkillsConfigWriteParams{}}},
                          backend::ProviderOperationValue{skillsWrite});
        typed::SkillsExtraRootsSetParams extraRoots;
        for (std::size_t index = 0; index < 300; ++index) {
            extraRoots.extraRoots.emplace_back(std::string(2048, 'p') + std::to_string(index));
        }
        completeOperation("skills/extraRoots/set",
                          backend::BackendCommand{backend::SkillsExtraRootsSet{std::move(extraRoots)}},
                          backend::ProviderOperationValue{typed::Unit{}});

        typed::GetAccountRateLimitsResponse rateLimitResult;
        typed::RateLimitResetCreditsSummary creditSummary;
        std::vector<typed::RateLimitResetCredit> credits(300);
        for (std::size_t index = 0; index < credits.size(); ++index) {
            credits[index].id = typed::RateLimitResetCreditId{std::string(2048, 'i') + std::to_string(index)};
            credits[index].description = std::string(2048, 'd');
            credits[index].title = std::string(2048, 't');
            credits[index].raw = Json{{"secret", std::string(2048, 'x')}};
        }
        creditSummary.credits = std::move(credits);
        creditSummary.raw = Json{{"secret", true}};
        rateLimitResult.rateLimitResetCredits = std::move(creditSummary);
        completeOperation("account/rateLimits/read",
                          backend::BackendCommand{backend::AccountRateLimitsRead{}},
                          backend::ProviderOperationValue{std::move(rateLimitResult)});

        typed::ConfigRequirementsReadResponse requirementsResult;
        typed::ConfigRequirements requirements;
        requirements.defaultPermissions = std::string(2048, 'd');
        std::map<typed::PermissionProfileName, bool> permissionProfiles;
        std::map<typed::ExperimentalFeatureId, bool> featureRequirements;
        for (std::size_t index = 0; index < 300; ++index) {
            permissionProfiles.emplace(typed::PermissionProfileName{std::string(2048, 'p') + std::to_string(index)}, true);
            featureRequirements.emplace(typed::ExperimentalFeatureId{std::string(2048, 'f') + std::to_string(index)}, false);
        }
        requirements.allowedPermissionProfiles = std::move(permissionProfiles);
        requirements.featureRequirements = std::move(featureRequirements);
        typed::NewThreadModelDefaults defaults;
        defaults.model = typed::ModelId{std::string(2048, 'm')};
        typed::ModelsRequirements modelRequirements;
        modelRequirements.newThread = std::move(defaults);
        modelRequirements.raw = Json{{"secret", true}};
        requirements.models = std::move(modelRequirements);
        requirements.raw = Json{{"secret", true}};
        requirementsResult.requirements = std::move(requirements);
        completeOperation("configRequirements/read",
                          backend::BackendCommand{backend::ConfigRequirementsRead{}},
                          backend::ProviderOperationValue{std::move(requirementsResult)});

        typed::PluginListResponse pluginList;
        pluginList.featuredPluginIds = std::vector<std::string>(300, std::string(canonicalReplacementStringLimit + 1, 'f'));
        std::vector<typed::MarketplaceLoadErrorInfo> loadErrors(300);
        for (typed::MarketplaceLoadErrorInfo& error : loadErrors) {
            error.marketplacePath = typed::AbsolutePath{std::string(2048, 'p')};
            error.message = std::string(canonicalReplacementStringLimit + 1, 'e');
            error.raw = Json{{"secret", true}};
        }
        pluginList.marketplaceLoadErrors = std::move(loadErrors);
        completeOperation("plugin/list",
                          backend::BackendCommand{backend::PluginList{typed::PluginListParams{}}},
                          backend::ProviderOperationValue{std::move(pluginList)});

        const typed::ThreadId goalThread{"goal-thread"};
        typed::ThreadGoalGetResponse oldGoalRead;
        typed::ThreadGoal oldGoal;
        oldGoal.threadId = goalThread;
        oldGoal.objective = "old objective";
        oldGoal.status = typed::ThreadGoalStatus::active();
        oldGoalRead.goal = oldGoal;
        operationState.conversations.latestGoalThreadId = goalThread;
        operationState.conversations.latestGoal = backend::ReplacementCache<typed::ThreadGoalGetResponse>{
            std::move(oldGoalRead), {}, {}, 0, false, {7, backend::Freshness::Current}};
        typed::ThreadGoal setGoal;
        setGoal.threadId = goalThread;
        setGoal.objective = std::string(64U * 1024U, 'g');
        setGoal.status = typed::ThreadGoalStatus::active();
        typed::ThreadGoalSetParams goalSetParams;
        goalSetParams.threadId = goalThread;
        typed::ThreadGoalSetResponse goalSetResponse;
        goalSetResponse.goal = setGoal;
        completeOperation("thread/goal/set",
                          backend::BackendCommand{backend::ThreadGoalSet{std::move(goalSetParams)}},
                          backend::ProviderOperationValue{std::move(goalSetResponse)});
        typed::ThreadGoalClearResponse goalClearResponse;
        goalClearResponse.cleared = true;
        completeOperation("thread/goal/clear",
                          backend::BackendCommand{backend::ThreadGoalClear{typed::ThreadGoalClearParams{goalThread}}},
                          backend::ProviderOperationValue{std::move(goalClearResponse)});
        typed::ThreadUnsubscribeResponse unsubscribe;
        unsubscribe.status = typed::ThreadUnsubscribeStatus::unsubscribed();
        completeOperation("thread/unsubscribe",
                          backend::BackendCommand{backend::ThreadUnsubscribe{typed::ThreadUnsubscribeParams{goalThread}}},
                          backend::ProviderOperationValue{unsubscribe});

        const backend::Snapshot typedDomainSnapshot = backend::makeSnapshot(operationState);
        const bool exactTypedCaches =
            operationState.configuration.lastWrite && operationState.configuration.lastWrite->value.raw.empty() &&
            operationState.configuration.lastWrite->value.filePath.value.size() == canonicalReplacementStringLimit &&
            operationState.configuration.configuration->stamp.freshness == backend::Freshness::Stale &&
            operationState.configuration.experimentalFeatureEnablement &&
            operationState.configuration.experimentalFeatureEnablement->value.enablement.size() == 256 &&
            operationState.integrations.marketplaceAdd && operationState.integrations.marketplaceRemove &&
            operationState.integrations.marketplaceUpgrade &&
            operationState.integrations.marketplaceUpgrade->value.selectedMarketplaces.size() == 256 &&
            operationState.integrations.marketplaceUpgrade->value.upgradedRoots.size() == 256 &&
            operationState.pluginsAndSkills.pluginInstall && operationState.pluginsAndSkills.pluginShareCheckout &&
            operationState.pluginsAndSkills.pluginShareSave && operationState.pluginsAndSkills.pluginShareUpdateTargets &&
            operationState.pluginsAndSkills.pluginShareUpdateTargets->value.principals.size() == 256 &&
            operationState.pluginsAndSkills.skillsConfigWrite && operationState.pluginsAndSkills.extraRoots &&
            operationState.pluginsAndSkills.extraRoots->roots.size() == 256 &&
            operationState.pluginsAndSkills.extraRoots->totalRoots == 300 && operationState.accounts.rateLimitRead &&
            operationState.accounts.rateLimitRead->value.rateLimitResetCredits.value->credits.value->size() == 256 &&
            operationState.accounts.rateLimitRead->value.rateLimitResetCredits.value->raw.empty() &&
            operationState.accounts.rateLimitRead->value.rateLimitResetCredits.value->credits.value->front().raw.empty() &&
            operationState.configuration.requirements &&
            operationState.configuration.requirements->value.requirements.value->allowedPermissionProfiles.value->size() == 256 &&
            operationState.configuration.requirements->value.requirements.value->featureRequirements.value->size() == 256 &&
            operationState.configuration.requirements->value.requirements.value->raw.empty() && operationState.pluginsAndSkills.plugins &&
            operationState.pluginsAndSkills.plugins->value.featuredPluginIds->size() == 256 &&
            operationState.pluginsAndSkills.plugins->value.featuredPluginIds->front().size() == canonicalReplacementStringLimit &&
            operationState.pluginsAndSkills.plugins->value.marketplaceLoadErrors->size() == 256 &&
            operationState.pluginsAndSkills.plugins->value.marketplaceLoadErrors->front().raw.empty() &&
            operationState.pluginsAndSkills.plugins->value.marketplaceLoadErrors->front().message.size() == canonicalReplacementStringLimit;
        const bool safeTypedSummaries =
            typedDomainSnapshot.configuration.lastWrite && typedDomainSnapshot.configuration.lastWrite->overridden &&
            typedDomainSnapshot.configuration.lastWrite->filePath.size() == backend::MaxSnapshotExtensionMethodBytes &&
            typedDomainSnapshot.configuration.experimentalFeatureEnablement &&
            typedDomainSnapshot.configuration.experimentalFeatureEnablement->totalEntries == 300 &&
            typedDomainSnapshot.integrations.marketplaceUpgrade && typedDomainSnapshot.integrations.marketplaceUpgrade->truncated &&
            typedDomainSnapshot.pluginsAndSkills.pluginShareSave &&
            typedDomainSnapshot.pluginsAndSkills.pluginShareSave->subjectId == "remote-plugin" &&
            typedDomainSnapshot.pluginsAndSkills.extraRoots && typedDomainSnapshot.pluginsAndSkills.extraRoots->truncated;
        const bool goalSemantics =
            operationState.conversations.latestGoalSetThreadId == goalThread &&
            operationState.conversations.latestGoalClearThreadId == goalThread &&
            operationState.conversations.latestUnsubscribeThreadId == goalThread && typedDomainSnapshot.conversations.latestGoalSet &&
            typedDomainSnapshot.conversations.latestGoalSet->objective->size() == canonicalReplacementStringLimit &&
            typedDomainSnapshot.conversations.latestGoalClear && typedDomainSnapshot.conversations.latestGoalClear->cleared == true &&
            typedDomainSnapshot.conversations.latestUnsubscribe &&
            typedDomainSnapshot.conversations.latestUnsubscribe->status == "unsubscribed" &&
            operationState.conversations.latestGoal->stamp.freshness == backend::Freshness::Stale;
        result.expectTrue(exactTypedCaches && safeTypedSummaries && goalSemantics,
                          "approved typed mutation results retain bounded exact caches, command associations, and safe semantic snapshots");

        backend::BackendState derivedBoundState = operationState;
        derivedBoundState.capacity.limits.maxSnapshotBytes = 4096;
        const backend::Snapshot derivedBoundSnapshot = backend::makeSnapshot(derivedBoundState);
        result.expectTrue(derivedBoundSnapshot.capacity.truncated && !derivedBoundSnapshot.capacity.mandatoryCoreExceedsLimit &&
                              !derivedBoundSnapshot.configuration.experimentalFeatureEnablement &&
                              !derivedBoundSnapshot.pluginsAndSkills.extraRoots && derivedBoundSnapshot.provider.generation == 7,
                          "snapshot bounding omits derived domain summaries deterministically before the mandatory-core fallback");

        reducer.apply(operationState, backend::ProviderConnectionInvalidated{7, "typed cache freshness test"});
        result.expectTrue(operationState.configuration.lastWrite->stamp.freshness == backend::Freshness::Stale &&
                              operationState.configuration.experimentalFeatureEnablement->stamp.freshness == backend::Freshness::Stale &&
                              operationState.integrations.marketplaceAdd->stamp.freshness == backend::Freshness::Stale &&
                              operationState.integrations.marketplaceRemove->stamp.freshness == backend::Freshness::Stale &&
                              operationState.integrations.marketplaceUpgrade->stamp.freshness == backend::Freshness::Stale &&
                              operationState.pluginsAndSkills.pluginInstall->stamp.freshness == backend::Freshness::Stale &&
                              operationState.pluginsAndSkills.pluginShareCheckout->stamp.freshness == backend::Freshness::Stale &&
                              operationState.pluginsAndSkills.pluginShareSave->stamp.freshness == backend::Freshness::Stale &&
                              operationState.pluginsAndSkills.pluginShareUpdateTargets->stamp.freshness == backend::Freshness::Stale &&
                              operationState.pluginsAndSkills.skillsConfigWrite->stamp.freshness == backend::Freshness::Stale &&
                              operationState.pluginsAndSkills.extraRoots->stamp.freshness == backend::Freshness::Stale &&
                              operationState.conversations.latestGoalSet->stamp.freshness == backend::Freshness::Stale &&
                              operationState.conversations.latestGoalClear->stamp.freshness == backend::Freshness::Stale &&
                              operationState.conversations.latestUnsubscribe->stamp.freshness == backend::Freshness::Stale,
                          "provider invalidation marks every approved typed mutation cache stale without dropping its bounded value");

        backend::BackendState itemState;
        itemState.provider.generation = 2;
        std::vector<typed::ThreadItem> items;
        typed::CollabAgentToolCallThreadItem collab;
        collab.metadata = metadata("all-items", "turn", "collab");
        collab.senderThreadId = typed::ThreadId{"sender"};
        collab.receiverThreadIds = {typed::ThreadId{"receiver"}};
        collab.status = typed::CollabAgentToolCallStatus::completed();
        collab.tool = typed::CollabAgentTool::spawnAgent();
        items.emplace_back(std::move(collab));
        typed::ContextCompactionThreadItem compact;
        compact.metadata = metadata("all-items", "turn", "compact");
        items.emplace_back(std::move(compact));
        typed::EnteredReviewModeThreadItem entered;
        entered.metadata = metadata("all-items", "turn", "entered");
        entered.review = "security";
        items.emplace_back(std::move(entered));
        typed::ExitedReviewModeThreadItem exited;
        exited.metadata = metadata("all-items", "turn", "exited");
        exited.review = "security";
        items.emplace_back(std::move(exited));
        typed::HookPromptThreadItem hook;
        hook.metadata = metadata("all-items", "turn", "hook");
        hook.fragments.push_back({"hook-run", "secret prompt text is not projected"});
        items.emplace_back(std::move(hook));
        typed::ImageGenerationThreadItem generated;
        generated.metadata = metadata("all-items", "turn", "generated");
        generated.result = "binary-image-data-is-not-projected";
        generated.status = "completed";
        items.emplace_back(std::move(generated));
        typed::ImageViewThreadItem viewed;
        viewed.metadata = metadata("all-items", "turn", "viewed");
        viewed.path = typed::PathString{"/synthetic/image.png"};
        items.emplace_back(std::move(viewed));
        typed::PlanThreadItem plan;
        plan.metadata = metadata("all-items", "turn", "plan");
        plan.text = "bounded plan";
        items.emplace_back(std::move(plan));
        typed::SleepThreadItem sleep;
        sleep.metadata = metadata("all-items", "turn", "sleep");
        sleep.durationMs = 25;
        items.emplace_back(std::move(sleep));
        typed::SubAgentActivityThreadItem subAgent;
        subAgent.metadata = metadata("all-items", "turn", "sub-agent");
        subAgent.agentPath = "root/worker";
        subAgent.agentThreadId = typed::ThreadId{"worker-thread"};
        subAgent.kind = typed::SubAgentActivityKind::started();
        items.emplace_back(std::move(subAgent));
        for (const typed::ThreadItem& item : items) {
            reducer.apply(itemState,
                          backend::ItemUpserted{
                              typed::ThreadId{"all-items"}, typed::TurnId{"turn"}, item, backend::ItemLifecycle::Completed, std::nullopt});
        }
        const backend::Snapshot itemSnapshot = backend::makeSnapshot(itemState);
        const std::vector<backend::ItemSnapshot>& projectedItems = itemSnapshot.threads.front().turns.front().items;
        const bool allTenUseful = projectedItems.size() == 10 && std::ranges::all_of(projectedItems, [](const backend::ItemSnapshot& item) {
                                      return item.data.is_object() && !item.data.empty() && !item.data.contains("codexType");
                                  });
        const std::string compactSnapshot =
            Json(projectedItems.front().data).dump() + itemSnapshot.threads.front().turns.front().items[5].data.dump();
        result.expectTrue(
            allTenUseful && compactSnapshot.find("secret prompt text") == std::string::npos &&
                compactSnapshot.find("binary-image-data") == std::string::npos,
            "all ten formerly generic ThreadItem alternatives have useful bounded projections without prompt or binary payloads");

        backend::BackendState boundedItemState;
        boundedItemState.provider.generation = 2;
        const std::string oversizedMethod(4096, 'i');
        const std::string oversizedPayload(64U * 1024U, 'p');
        typed::CommandExecutionThreadItem boundedCommand;
        boundedCommand.metadata = metadata("bounded-items", "turn", "command");
        boundedCommand.command = oversizedPayload;
        boundedCommand.cwd = typed::PathString{oversizedMethod};
        boundedCommand.status = typed::CommandExecutionStatus{oversizedMethod};
        boundedCommand.processId = oversizedMethod;
        typed::McpToolCallThreadItem boundedMcp;
        boundedMcp.metadata = metadata("bounded-items", "turn", "mcp");
        boundedMcp.server = oversizedMethod;
        boundedMcp.tool = oversizedMethod;
        boundedMcp.status = typed::McpToolCallStatus{oversizedMethod};
        typed::DynamicToolCallThreadItem boundedDynamic;
        boundedDynamic.metadata = metadata("bounded-items", "turn", "dynamic");
        boundedDynamic.tool = oversizedMethod;
        boundedDynamic.status = typed::DynamicToolCallStatus{oversizedMethod};
        boundedDynamic.nameSpace = oversizedMethod;
        typed::WebSearchThreadItem boundedWeb;
        boundedWeb.metadata = metadata("bounded-items", "turn", "web");
        boundedWeb.query = oversizedPayload;
        typed::FileChangeThreadItem boundedFileChange;
        boundedFileChange.metadata = metadata("bounded-items", "turn", "file");
        boundedFileChange.metadata.raw["changes"] = Json::array({Json{{"rawSentinel", "must-not-appear"}}});
        boundedFileChange.status = typed::PatchApplyStatus::completed();
        boundedFileChange.changes.push_back(
            typed::FileUpdateChange{std::string(64U * 1024U, 'd'), typed::UpdatePatchChangeKind{}, "/secret/path"});
        const std::vector<typed::ThreadItem> boundedItems{boundedCommand, boundedMcp, boundedDynamic, boundedWeb, boundedFileChange};
        for (const typed::ThreadItem& item : boundedItems) {
            reducer.apply(
                boundedItemState,
                backend::ItemUpserted{
                    typed::ThreadId{"bounded-items"}, typed::TurnId{"turn"}, item, backend::ItemLifecycle::Completed, std::nullopt});
        }
        const backend::Snapshot boundedItemSnapshot = backend::makeSnapshot(boundedItemState);
        const auto& boundedItemProjections = boundedItemSnapshot.threads.front().turns.front().items;
        const bool boundedItemStrings =
            boundedItemProjections.size() == 5 &&
            boundedItemProjections[0].data.at("command").get_ref<const std::string&>().size() ==
                backend::MaxSnapshotExtensionPayloadBytes &&
            boundedItemProjections[0].data.at("cwd").get_ref<const std::string&>().size() == backend::MaxSnapshotExtensionMethodBytes &&
            boundedItemProjections[0].data.at("processId").get_ref<const std::string&>().size() ==
                backend::MaxSnapshotExtensionMethodBytes &&
            boundedItemProjections[1].data.at("tool").get_ref<const std::string&>().size() == backend::MaxSnapshotExtensionMethodBytes &&
            boundedItemProjections[1].data.at("server").get_ref<const std::string&>().size() == backend::MaxSnapshotExtensionMethodBytes &&
            boundedItemProjections[2].data.at("namespace").get_ref<const std::string&>().size() ==
                backend::MaxSnapshotExtensionMethodBytes &&
            boundedItemProjections[3].data.at("query").get_ref<const std::string&>().size() == backend::MaxSnapshotExtensionPayloadBytes &&
            boundedItemProjections[4].data.value("changeCount", 0) == 1 &&
            boundedItemProjections[4].data.at("changes").front().value("pathRedacted", false) &&
            boundedItemProjections[4].data.at("changes").front().value("diffOmitted", false) &&
            boundedItemProjections[4].data.dump().find("must-not-appear") == std::string::npos &&
            boundedItemProjections[4].data.dump().find("/secret/path") == std::string::npos;
        result.expectTrue(boundedItemStrings,
                          "safe ThreadItem projections bound command, path, process, MCP, dynamic-tool, and web-search strings");

        backend::BackendState capacityState;
        backend::BackendCapacityOptions zeroLimits;
        zeroLimits.maxRetainedProcesses = 0;
        zeroLimits.maxRetainedFilesystemWatches = 0;
        zeroLimits.maxRetainedFuzzySearchSessions = 0;
        reducer.apply(capacityState, backend::CapacityConfigured{zeroLimits});
        backend::detail::resetRetentionCapacityInstrumentation();
        const backend::Reduction deniedProcess = reducer.apply(
            capacityState,
            backend::ProviderResourceAdmissionRequested{backend::ProviderResourceKind::Process, "process-reservation", "process-zero"});
        const backend::Reduction deniedWatch = reducer.apply(
            capacityState,
            backend::ProviderResourceAdmissionRequested{backend::ProviderResourceKind::FilesystemWatch, "watch-reservation", "watch-zero"});
        const backend::Reduction deniedSearch = reducer.apply(
            capacityState,
            backend::ProviderResourceAdmissionRequested{backend::ProviderResourceKind::FuzzySearch, "search-reservation", "search-zero"});
        const backend::detail::RetentionCapacityInstrumentation instrumentation = backend::detail::retentionCapacityInstrumentation();
        const backend::Snapshot capacitySnapshot = backend::makeSnapshot(capacityState);
        result.expectTrue(
            deniedProcess.resourceAdmission == false && deniedWatch.resourceAdmission == false && deniedSearch.resourceAdmission == false &&
                capacityState.capacity.rejectedOperations == 3 && capacitySnapshot.capacity.retainedProcesses == 0 &&
                capacitySnapshot.capacity.retainedFilesystemWatches == 0 && capacitySnapshot.capacity.retainedFuzzySearchSessions == 0 &&
                instrumentation.slowPathEntries == 0 && instrumentation.pendingReferenceBuilds == 0,
            "zero resource capacities reject globally, expose incremental counters, and keep the ordinary capacity path O(1)");

        backend::BackendState targetBoundState;
        targetBoundState.provider.generation = 3;
        backend::BackendCapacityOptions targetBoundLimits;
        targetBoundLimits.maxRetainedProcesses = 1;
        targetBoundLimits.maxRetainedFilesystemWatches = 1;
        targetBoundLimits.maxRetainedFuzzySearchSessions = 1;
        reducer.apply(targetBoundState, backend::CapacityConfigured{targetBoundLimits});
        const auto applyTargetBoundTyped = [&reducer, &targetBoundState](typed::Event event) {
            const std::vector<backend::BackendEvent> translated = reducer.translate(event);
            return reducer.apply(targetBoundState, translated.front());
        };
        reducer.apply(targetBoundState,
                      backend::ProviderResourceAdmissionRequested{backend::ProviderResourceKind::Process, "process-a-op", "process-a"});
        reducer.apply(targetBoundState,
                      backend::ProviderResourceAdmissionRequested{backend::ProviderResourceKind::FilesystemWatch, "watch-a-op", "watch-a"});
        reducer.apply(targetBoundState,
                      backend::ProviderResourceAdmissionRequested{backend::ProviderResourceKind::FuzzySearch, "search-a-op", "search-a"});
        typed::ProcessOutputDeltaNotification unsolicitedProcess;
        unsolicitedProcess.processHandle = "process-b";
        unsolicitedProcess.stream = typed::ProcessOutputStream::stdoutStream();
        unsolicitedProcess.deltaBase64 = "unexpected";
        unsolicitedProcess.raw = Json{{"params", Json::object()}};
        const backend::Reduction processTargetRejected = applyTargetBoundTyped(typed::Event{std::move(unsolicitedProcess)});
        typed::FsChangedNotification unsolicitedWatch;
        unsolicitedWatch.watchId = typed::FsWatchId{"watch-b"};
        unsolicitedWatch.raw = Json{{"params", Json::object()}};
        const backend::Reduction watchTargetRejected = applyTargetBoundTyped(typed::Event{std::move(unsolicitedWatch)});
        typed::FuzzyFileSearchSessionUpdatedNotification unsolicitedSearch;
        unsolicitedSearch.sessionId = "search-b";
        unsolicitedSearch.query = "unexpected";
        unsolicitedSearch.raw = Json{{"params", Json::object()}};
        const backend::Reduction searchTargetRejected = applyTargetBoundTyped(typed::Event{std::move(unsolicitedSearch)});
        const bool unrelatedResourcesCannotSteal =
            processTargetRejected.providerCapacityFailure && watchTargetRejected.providerCapacityFailure &&
            searchTargetRejected.providerCapacityFailure && targetBoundState.processes.empty() &&
            targetBoundState.filesystemWatches.empty() && targetBoundState.fuzzySearchSessions.empty() &&
            targetBoundState.processReservationClaims.empty() && targetBoundState.filesystemWatchReservationClaims.empty() &&
            targetBoundState.fuzzySearchReservationClaims.empty() &&
            targetBoundState.processReservationTargets.at("process-a-op") == "process-a" &&
            targetBoundState.filesystemWatchReservationTargets.at("watch-a-op") == "watch-a" &&
            targetBoundState.fuzzySearchReservationTargets.at("search-a-op") == "search-a";

        typed::ProcessOutputDeltaNotification expectedProcess;
        expectedProcess.processHandle = "process-a";
        expectedProcess.stream = typed::ProcessOutputStream::stdoutStream();
        expectedProcess.deltaBase64 = "expected";
        expectedProcess.raw = Json{{"params", Json::object()}};
        const backend::Reduction processTargetAdmitted = applyTargetBoundTyped(typed::Event{std::move(expectedProcess)});
        typed::FsChangedNotification expectedWatch;
        expectedWatch.watchId = typed::FsWatchId{"watch-a"};
        expectedWatch.raw = Json{{"params", Json::object()}};
        const backend::Reduction watchTargetAdmitted = applyTargetBoundTyped(typed::Event{std::move(expectedWatch)});
        typed::FuzzyFileSearchSessionUpdatedNotification expectedSearch;
        expectedSearch.sessionId = "search-a";
        expectedSearch.query = "expected";
        expectedSearch.raw = Json{{"params", Json::object()}};
        const backend::Reduction searchTargetAdmitted = applyTargetBoundTyped(typed::Event{std::move(expectedSearch)});
        const bool exactTargetsClaimReservations =
            !processTargetAdmitted.providerCapacityFailure && !watchTargetAdmitted.providerCapacityFailure &&
            !searchTargetAdmitted.providerCapacityFailure && targetBoundState.processes.contains("process-a") &&
            targetBoundState.filesystemWatches.contains("watch-a") && targetBoundState.fuzzySearchSessions.contains("search-a") &&
            targetBoundState.processReservationClaims.at("process-a") == "process-a-op" &&
            targetBoundState.filesystemWatchReservationClaims.at("watch-a") == "watch-a-op" &&
            targetBoundState.fuzzySearchReservationClaims.at("search-a") == "search-a-op";
        reducer.apply(targetBoundState, backend::ProviderConnectionInvalidated{3, "target-bound reservation test"});
        const bool targetReservationsInvalidated =
            targetBoundState.processReservations.empty() && targetBoundState.processReservationTargets.empty() &&
            targetBoundState.processReservationClaims.empty() && targetBoundState.filesystemWatchReservations.empty() &&
            targetBoundState.filesystemWatchReservationTargets.empty() && targetBoundState.filesystemWatchReservationClaims.empty() &&
            targetBoundState.fuzzySearchReservations.empty() && targetBoundState.fuzzySearchReservationTargets.empty() &&
            targetBoundState.fuzzySearchReservationClaims.empty();
        result.expectTrue(unrelatedResourcesCannotSteal && exactTargetsClaimReservations && targetReservationsInvalidated,
                          "provider-resource reservations are target-bound and cannot be stolen by unsolicited resources");

        backend::ReducerOptions boundedOptions;
        boundedOptions.maxNoticeSummaryBytes = 8;
        boundedOptions.maxNoticeDetailsBytes = 8;
        backend::Reducer boundedReducer{boundedOptions};
        backend::BackendState boundedState;
        boundedState.provider.generation = 4;
        backend::BackendCapacityOptions boundedLimits;
        boundedLimits.maxRetainedNotices = 1;
        boundedLimits.maxRetainedProcesses = 1;
        boundedLimits.maxProcessOutputBytesPerProcess = 4;
        boundedLimits.maxAccumulatedProcessOutputBytes = 3;
        boundedLimits.maxRetainedFilesystemWatches = 1;
        boundedLimits.maxRetainedFuzzySearchSessions = 1;
        boundedLimits.maxRetainedActivityRecords = 1;
        boundedReducer.apply(boundedState, backend::CapacityConfigured{boundedLimits});

        const auto applyTyped = [&boundedReducer, &boundedState](typed::Event event) {
            const std::vector<backend::BackendEvent> translated = boundedReducer.translate(event);
            return boundedReducer.apply(boundedState, translated.front());
        };

        typed::WarningNotification firstWarning;
        firstWarning.message = "first warning";
        firstWarning.raw = Json{{"params", Json::object()}};
        applyTyped(typed::Event{std::move(firstWarning)});
        typed::WarningNotification secondWarning;
        secondWarning.message = "second warning";
        secondWarning.raw = Json{{"params", Json::object()}};
        applyTyped(typed::Event{std::move(secondWarning)});
        const bool noticeBounded = boundedState.notices.size() == 1 && boundedState.capacity.retainedNotices == 1 &&
                                   boundedState.capacity.evictedNotices == 1 && boundedState.notices.front().summary == "second w";

        const backend::Reduction processReservation = boundedReducer.apply(
            boundedState, backend::ProviderResourceAdmissionRequested{backend::ProviderResourceKind::Process, "process-op", "process-1"});
        const bool reservationNotRetained = processReservation.resourceAdmission == true && boundedState.capacity.retainedProcesses == 0;
        typed::ProcessOutputDeltaNotification stdoutDelta;
        stdoutDelta.processHandle = "process-1";
        stdoutDelta.stream = typed::ProcessOutputStream::stdoutStream();
        stdoutDelta.deltaBase64 = "abcd";
        stdoutDelta.raw = Json{{"params", Json::object()}};
        const backend::Reduction earlyProcess = applyTyped(typed::Event{std::move(stdoutDelta)});
        typed::ProcessOutputDeltaNotification stderrDelta;
        stderrDelta.processHandle = "process-1";
        stderrDelta.stream = typed::ProcessOutputStream::stderrStream();
        stderrDelta.deltaBase64 = "efgh";
        stderrDelta.raw = Json{{"params", Json::object()}};
        applyTyped(typed::Event{std::move(stderrDelta)});
        const backend::ProcessState& boundedProcess = boundedState.processes.at("process-1");
        const bool processOutputBounded = !earlyProcess.providerCapacityFailure && boundedState.capacity.retainedProcesses == 1 &&
                                          boundedState.processReservationClaims.contains("process-1") &&
                                          boundedProcess.stdoutData.size() + boundedProcess.stderrData.size() == 3 &&
                                          boundedProcess.stdoutTruncated && boundedProcess.stderrTruncated &&
                                          boundedProcess.droppedOutputBytes == 5 && boundedState.capacity.droppedProcessOutputBytes == 5;
        const backend::Reduction activeProcessDenied = boundedReducer.apply(
            boundedState,
            backend::ProviderResourceAdmissionRequested{backend::ProviderResourceKind::Process, "second-process-op", "process-2"});
        typed::ProcessExitedNotification processExited;
        processExited.processHandle = "process-1";
        processExited.exitCode = 0;
        processExited.stdout = "xy";
        processExited.raw = Json{{"params", Json::object()}};
        applyTyped(typed::Event{std::move(processExited)});
        const backend::Reduction pendingTerminalProcessProtected = boundedReducer.apply(
            boundedState,
            backend::ProviderResourceAdmissionRequested{backend::ProviderResourceKind::Process, "second-process-op", "process-2"});
        boundedReducer.apply(boundedState,
                             backend::ProviderResourceAdmissionReleased{backend::ProviderResourceKind::Process, "process-op"});
        const backend::Reduction terminalProcessEvicted = boundedReducer.apply(
            boundedState,
            backend::ProviderResourceAdmissionRequested{backend::ProviderResourceKind::Process, "second-process-op", "process-2"});
        boundedReducer.apply(boundedState,
                             backend::ProviderResourceAdmissionReleased{backend::ProviderResourceKind::Process, "second-process-op"});
        const backend::Reduction duplicateProcessRelease = boundedReducer.apply(
            boundedState, backend::ProviderResourceAdmissionReleased{backend::ProviderResourceKind::Process, "second-process-op"});
        const bool processCapacity = activeProcessDenied.resourceAdmission == false &&
                                     pendingTerminalProcessProtected.resourceAdmission == false &&
                                     terminalProcessEvicted.resourceAdmission == true && boundedState.capacity.evictedProcesses == 1 &&
                                     boundedState.capacity.retainedProcesses == 0 && !duplicateProcessRelease.changed;

        const backend::Reduction watchReservation = boundedReducer.apply(
            boundedState,
            backend::ProviderResourceAdmissionRequested{backend::ProviderResourceKind::FilesystemWatch, "watch-op", "watch-1"});
        typed::FsChangedNotification changed;
        changed.watchId = typed::FsWatchId{"watch-1"};
        for (std::size_t index = 0; index < 300; ++index) {
            changed.changedPaths.emplace_back("/changed/path/" + std::to_string(index));
        }
        changed.raw = Json{{"params", Json::object()}};
        const backend::Reduction earlyWatch = applyTyped(typed::Event{std::move(changed)});
        typed::FsWatchParams watchParams{typed::AbsolutePath{"/bounded/watch/root"}, typed::FsWatchId{"watch-1"}};
        typed::FsWatchResponse watchResponse;
        watchResponse.path = watchParams.path;
        boundedReducer.apply(boundedState,
                             backend::ProviderOperationCompleted{"fs/watch",
                                                                 backend::BackendCommand{backend::FsWatch{watchParams}},
                                                                 backend::ProviderOperationValue{watchResponse},
                                                                 std::optional<std::string>{"watch-op"}});
        const bool watchBounded = watchReservation.resourceAdmission == true && !earlyWatch.providerCapacityFailure &&
                                  boundedState.capacity.retainedFilesystemWatches == 1 &&
                                  boundedState.filesystemWatches.at("watch-1").changedPaths.size() == 256 &&
                                  boundedState.filesystemWatches.at("watch-1").root.has_value() &&
                                  boundedState.filesystemWatches.at("watch-1").root->value.size() == 8;
        boundedReducer.apply(boundedState,
                             backend::ProviderOperationCompleted{
                                 "fs/unwatch",
                                 backend::BackendCommand{backend::FsUnwatch{typed::FsUnwatchParams{typed::FsWatchId{"watch-1"}}}},
                                 backend::ProviderOperationValue{typed::Unit{}},
                                 std::nullopt});
        const bool watchReleased = boundedState.capacity.retainedFilesystemWatches == 0 && boundedState.filesystemWatches.empty();

        const backend::Reduction fuzzyReservation = boundedReducer.apply(
            boundedState, backend::ProviderResourceAdmissionRequested{backend::ProviderResourceKind::FuzzySearch, "fuzzy-op", "fuzzy-1"});
        typed::FuzzyFileSearchSessionUpdatedNotification fuzzy;
        fuzzy.sessionId = "fuzzy-1";
        fuzzy.query = "query that is deliberately long";
        fuzzy.files.resize(600);
        for (std::size_t index = 0; index < fuzzy.files.size(); ++index) {
            fuzzy.files[index].fileName = "file-name-" + std::to_string(index);
            fuzzy.files[index].path = "/very/long/path/" + std::to_string(index);
            fuzzy.files[index].root = "/very/long/root";
        }
        fuzzy.raw = Json{{"params", Json::object()}};
        const backend::Reduction earlyFuzzy = applyTyped(typed::Event{std::move(fuzzy)});
        typed::FuzzyFileSearchSessionCompletedNotification fuzzyCompleted;
        fuzzyCompleted.sessionId = "fuzzy-1";
        fuzzyCompleted.raw = Json{{"params", Json::object()}};
        applyTyped(typed::Event{std::move(fuzzyCompleted)});
        const bool fuzzyBounded = fuzzyReservation.resourceAdmission == true && !earlyFuzzy.providerCapacityFailure &&
                                  boundedState.capacity.retainedFuzzySearchSessions == 1 &&
                                  boundedState.fuzzySearchSessions.at("fuzzy-1").query.size() == 8 &&
                                  boundedState.fuzzySearchSessions.at("fuzzy-1").files.size() == 512 &&
                                  boundedState.fuzzySearchSessions.at("fuzzy-1").files.front().path.size() == 8;
        const backend::Reduction pendingCompletedFuzzyProtected = boundedReducer.apply(
            boundedState,
            backend::ProviderResourceAdmissionRequested{backend::ProviderResourceKind::FuzzySearch, "next-fuzzy-op", "fuzzy-2"});
        boundedReducer.apply(boundedState,
                             backend::ProviderResourceAdmissionReleased{backend::ProviderResourceKind::FuzzySearch, "fuzzy-op"});
        const backend::Reduction nextFuzzyReservation = boundedReducer.apply(
            boundedState,
            backend::ProviderResourceAdmissionRequested{backend::ProviderResourceKind::FuzzySearch, "next-fuzzy-op", "fuzzy-2"});
        boundedReducer.apply(boundedState,
                             backend::ProviderResourceAdmissionReleased{backend::ProviderResourceKind::FuzzySearch, "next-fuzzy-op"});
        const bool fuzzyEvicted = pendingCompletedFuzzyProtected.resourceAdmission == false &&
                                  nextFuzzyReservation.resourceAdmission == true && boundedState.capacity.evictedFuzzySearchSessions == 1 &&
                                  boundedState.capacity.retainedFuzzySearchSessions == 0;

        typed::ExternalAgentConfigImportProgressNotification importOne;
        importOne.importId = "import-one";
        importOne.raw = Json{{"params", Json::object()}};
        applyTyped(typed::Event{std::move(importOne)});
        typed::ExternalAgentConfigImportProgressNotification importTwo;
        importTwo.importId = "import-two";
        importTwo.raw = Json{{"params", Json::object()}};
        applyTyped(typed::Event{std::move(importTwo)});
        const auto hasImportActivity = [&boundedState](std::string_view subject) {
            return std::ranges::any_of(boundedState.activities, [subject](const auto& entry) {
                return entry.second.kind == "external_agent_import" && entry.second.subjectId == subject;
            });
        };
        const bool activeActivityProtected = boundedState.activities.size() == 1 && hasImportActivity("import-one");
        typed::ExternalAgentConfigImportCompletedNotification importOneCompleted;
        importOneCompleted.importId = "import-one";
        importOneCompleted.raw = Json{{"params", Json::object()}};
        applyTyped(typed::Event{std::move(importOneCompleted)});
        typed::ExternalAgentConfigImportProgressNotification importTwoAgain;
        importTwoAgain.importId = "import-two";
        importTwoAgain.raw = Json{{"params", Json::object()}};
        applyTyped(typed::Event{std::move(importTwoAgain)});
        const bool activityEvicted = boundedState.activities.size() == 1 && hasImportActivity("import-two") &&
                                     boundedState.capacity.retainedActivityRecords == 1 &&
                                     boundedState.capacity.evictedActivityRecords == 2;

        const backend::Snapshot beforeInvalidation = backend::makeSnapshot(boundedState);
        const auto retainedBeforeInvalidation = std::tuple{boundedState.capacity.retainedNotices,
                                                           boundedState.capacity.retainedProcesses,
                                                           boundedState.capacity.retainedFilesystemWatches,
                                                           boundedState.capacity.retainedFuzzySearchSessions,
                                                           boundedState.capacity.retainedActivityRecords};
        boundedReducer.apply(boundedState, backend::ProviderConnectionInvalidated{4, "capacity test"});
        const backend::Snapshot afterInvalidation = backend::makeSnapshot(boundedState);
        const auto retainedAfterInvalidation = std::tuple{boundedState.capacity.retainedNotices,
                                                          boundedState.capacity.retainedProcesses,
                                                          boundedState.capacity.retainedFilesystemWatches,
                                                          boundedState.capacity.retainedFuzzySearchSessions,
                                                          boundedState.capacity.retainedActivityRecords};
        boundedState.capacity.droppedProcessOutputBytes = std::numeric_limits<std::uint64_t>::max();
        boundedReducer.apply(boundedState, backend::CapacityChanged{backend::CapacityMetric::DroppedProcessOutputBytes, std::uint64_t{1}});
        const bool countersStable = beforeInvalidation.capacity.retainedNotices == 1 &&
                                    retainedBeforeInvalidation == retainedAfterInvalidation &&
                                    beforeInvalidation.capacity.retainedNotices == afterInvalidation.capacity.retainedNotices &&
                                    boundedState.capacity.droppedProcessOutputBytes == std::numeric_limits<std::uint64_t>::max();

        result.expectTrue(
            noticeBounded && reservationNotRetained && processOutputBounded && processCapacity,
            "notice/process capacities preserve active resources, evict terminal state, bound combined output, and saturate drops");
        result.expectTrue(watchBounded && watchReleased && fuzzyBounded && fuzzyEvicted,
                          "watch and fuzzy reservations promote early notifications without false overflow and retain bounded summaries");
        result.expectTrue(
            activeActivityProtected && activityEvicted && countersStable,
            "activity bounds protect active work while provider invalidation and snapshots preserve canonical incremental counts");

        backend::BackendState resolvedState;
        resolvedState.provider.generation = 3;
        resolvedState.pendingRequests.emplace(
            backend::PendingRequestId{77},
            backend::PendingRequestState{backend::PendingRequestId{77}, typed::TypedServerRequest{approvalRequest()}, 3});
        typed::ServerRequestResolvedNotification resolved;
        resolved.requestId = ServerRequestId{std::string("server-request")};
        resolved.threadId = typed::ThreadId{"thread-request"};
        resolved.raw = Json{{"params", Json::object()}};
        const std::vector<backend::BackendEvent> resolvedEvents = reducer.translate(typed::Event{resolved});
        const backend::Reduction resolvedReduction = reducer.apply(resolvedState, resolvedEvents.front());
        const backend::Reduction duplicateReduction = reducer.apply(resolvedState, resolvedEvents.front());
        const backend::Snapshot resolvedSnapshot = backend::makeSnapshot(resolvedState);
        result.expectTrue(resolvedState.pendingRequests.empty() && resolvedReduction.pendingRequestRemovals.size() == 1 &&
                              resolvedReduction.pendingRequestRemovals.front().reason == "externally_resolved" &&
                              !duplicateReduction.changed && duplicateReduction.pendingRequestRemovals.empty() &&
                              resolvedSnapshot.recentExtensions.empty() && resolvedSnapshot.conversations.latestNotificationMethods.empty(),
                          "serverRequest/resolved retires a matching occurrence exactly once without retaining its provider request id");

        backend::BackendState staleResolvedState;
        staleResolvedState.provider.generation = 3;
        staleResolvedState.sequence = backend::SequenceNumber{41};
        staleResolvedState.pendingRequests.emplace(
            backend::PendingRequestId{79},
            backend::PendingRequestState{backend::PendingRequestId{79}, typed::TypedServerRequest{approvalRequest()}, 2});
        const backend::Reduction staleResolvedReduction = reducer.apply(staleResolvedState, resolvedEvents.front());
        const backend::Snapshot staleResolvedSnapshot = backend::makeSnapshot(staleResolvedState);
        result.expectTrue(
            !staleResolvedReduction.changed && staleResolvedReduction.pendingRequestRemovals.empty() &&
                staleResolvedState.sequence == backend::SequenceNumber{41} && staleResolvedState.pendingRequests.size() == 1 &&
                staleResolvedState.pendingRequests.contains(backend::PendingRequestId{79}) &&
                staleResolvedSnapshot.recentExtensions.empty() && staleResolvedSnapshot.conversations.latestNotificationMethods.empty(),
            "serverRequest/resolved with a stale occurrence generation is an exact ownership and sequence no-op");

        backend::BackendState unknownResolvedState;
        unknownResolvedState.provider.generation = 3;
        unknownResolvedState.sequence = backend::SequenceNumber{42};
        unknownResolvedState.pendingRequests.emplace(
            backend::PendingRequestId{80},
            backend::PendingRequestState{backend::PendingRequestId{80}, typed::TypedServerRequest{approvalRequest()}, 3});
        typed::ServerRequestResolvedNotification unknownResolved = resolved;
        unknownResolved.requestId = ServerRequestId{std::string{"unknown-provider-request"}};
        const std::vector<backend::BackendEvent> unknownResolvedEvents = reducer.translate(typed::Event{std::move(unknownResolved)});
        const backend::Reduction unknownResolvedReduction = reducer.apply(unknownResolvedState, unknownResolvedEvents.front());
        const backend::Snapshot unknownResolvedSnapshot = backend::makeSnapshot(unknownResolvedState);
        result.expectTrue(
            !unknownResolvedReduction.changed && unknownResolvedReduction.pendingRequestRemovals.empty() &&
                unknownResolvedState.sequence == backend::SequenceNumber{42} && unknownResolvedState.pendingRequests.size() == 1 &&
                unknownResolvedState.pendingRequests.contains(backend::PendingRequestId{80}) &&
                unknownResolvedSnapshot.recentExtensions.empty() && unknownResolvedSnapshot.conversations.latestNotificationMethods.empty(),
            "serverRequest/resolved with an unknown provider request id leaves another current occurrence untouched");

        backend::BackendState conflictingResolvedState;
        conflictingResolvedState.provider.generation = 3;
        typed::CommandApprovalRequest conflictingRequest = approvalRequest();
        conflictingRequest.threadId = typed::ThreadId{"different-thread"};
        conflictingResolvedState.pendingRequests.emplace(
            backend::PendingRequestId{78},
            backend::PendingRequestState{backend::PendingRequestId{78}, typed::TypedServerRequest{std::move(conflictingRequest)}, 3});
        reducer.apply(conflictingResolvedState, resolvedEvents.front());
        const backend::Snapshot conflictingResolvedSnapshot = backend::makeSnapshot(conflictingResolvedState);
        result.expectTrue(
            conflictingResolvedState.pendingRequests.size() == 1 && conflictingResolvedSnapshot.recentExtensions.size() == 1 &&
                conflictingResolvedSnapshot.recentExtensions.front().payload.dump().find("server-request") == std::string::npos,
            "a conflicting resolved notification remains bounded and omits the provider request identifier");

        backend::BackendState pendingKinds;
        pendingKinds.pendingRequests.emplace(
            backend::PendingRequestId{1},
            backend::PendingRequestState{
                backend::PendingRequestId{1}, typed::TypedServerRequest{deferredRequest<typed::ApplyPatchApprovalRequest>(1)}, 1});
        pendingKinds.pendingRequests.emplace(
            backend::PendingRequestId{2},
            backend::PendingRequestState{
                backend::PendingRequestId{2}, typed::TypedServerRequest{deferredRequest<typed::ExecCommandApprovalRequest>(2)}, 1});
        pendingKinds.pendingRequests.emplace(
            backend::PendingRequestId{3},
            backend::PendingRequestState{
                backend::PendingRequestId{3}, typed::TypedServerRequest{deferredRequest<typed::PermissionsApprovalRequest>(3)}, 1});
        pendingKinds.pendingRequests.emplace(
            backend::PendingRequestId{4},
            backend::PendingRequestState{
                backend::PendingRequestId{4}, typed::TypedServerRequest{deferredRequest<typed::AttestationGenerateRequest>(4)}, 1});
        pendingKinds.pendingRequests.emplace(
            backend::PendingRequestId{5},
            backend::PendingRequestState{
                backend::PendingRequestId{5}, typed::TypedServerRequest{deferredRequest<typed::DynamicToolCallRequest>(5)}, 1});
        pendingKinds.pendingRequests.emplace(
            backend::PendingRequestId{6},
            backend::PendingRequestState{
                backend::PendingRequestId{6}, typed::TypedServerRequest{deferredRequest<typed::McpServerElicitationRequest>(6)}, 1});
        const backend::Snapshot pendingKindsSnapshot = backend::makeSnapshot(pendingKinds);
        const std::vector<std::string> expectedKinds{
            "apply_patch_approval", "exec_command_approval", "permissions_approval", "attestation", "dynamic_tool_call", "mcp_elicitation"};
        const bool kindsMatch =
            pendingKindsSnapshot.pendingRequests.size() == expectedKinds.size() &&
            std::ranges::equal(
                pendingKindsSnapshot.pendingRequests, expectedKinds, {}, &backend::PendingRequestSnapshot::type, std::identity{});
        result.expectTrue(kindsMatch, "all six formerly generic pending request kinds have explicit bounded safe snapshot identities");
        result.expectTrue(pendingKindsSnapshot.pendingRequests[0].details.value("method", "") == "applyPatchApproval" &&
                              pendingKindsSnapshot.pendingRequests[0].details.contains("summary") &&
                              pendingKindsSnapshot.pendingRequests[1].details.value("method", "") == "execCommandApproval" &&
                              pendingKindsSnapshot.pendingRequests[2].details.value("method", "") == "item/permissions/requestApproval",
                          "approval snapshots retain the bounded/redacted legacy generic envelope beside a meaningful backend summary");
    }
} // namespace

int main() {
    tests::support::TestResult result;

    testInitialStateAndLifecycle(result);
    testThreadAndTurnHydration(result);
    testStatusOnlyPlaceholderParity(result);
    testItemsAndHighVolumeDeltas(result);
    testCompletionFailureAndAuxiliaryUpdates(result);
    testUserMessageLifecycle(result);
    testUserMessageSnapshotBounding(result);
    testUnknownItemCommonMetadataFallbacks(result);
    testUnknownPreservationAndTranslation(result);
    testPendingRequestsAndSessions(result);
    testSnapshotDeterminism(result);
    testA16bNotificationItemAndCapacityClosure(result);

    return result.processResult();
}
