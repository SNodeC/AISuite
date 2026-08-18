#include "CodexBackendTestSupport.h"
#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/backend/internal/RecoveryPolicy.h"
#include "ai/openai/codex/backend/internal/RetentionCapacityInstrumentation.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
    namespace backend = ai::openai::codex::backend;
    namespace typed = ai::openai::codex::typed;

    using ai::openai::codex::Error;
    using ai::openai::codex::Json;
    using ai::openai::codex::detail::TransportCallbacks;

    using FakeBackendCore = backend::BackendCore<tests::codex::FakeAppServerClient>;

    void testRecoveryPolicyEligibility(tests::support::TestResult& result) {
        backend::ProviderState provider;
        provider.desiredRunning = true;
        backend::RecoveryOptions options;
        options.enabled = true;
        options.maximumAttempts = 1;

        result.expectTrue(!backend::detail::isAutomaticRecoveryEligible(provider, options, std::nullopt),
                          "a provider failure without a classified Error is not eligible for automatic recovery");
        result.expectTrue(backend::detail::isAutomaticRecoveryEligible(
                              provider, options, Error{Error::Category::Transport, 1, "synthetic transport failure"}) &&
                              backend::detail::isAutomaticRecoveryEligible(
                                  provider, options, Error{Error::Category::Process, 2, "synthetic process failure"}),
                          "only classified Transport and Process failures are eligible while attempts remain");
        result.expectTrue(!backend::detail::isAutomaticRecoveryEligible(
                              provider, options, Error{Error::Category::Protocol, 3, "synthetic protocol failure"}),
                          "a classified nonretryable failure is ineligible for automatic recovery");
        provider.recovery.attempts = 1;
        result.expectTrue(!backend::detail::isAutomaticRecoveryEligible(
                              provider, options, Error{Error::Category::Transport, 4, "synthetic exhausted failure"}),
                          "a finite recovery policy rejects failures after its attempt limit is exhausted");
        options.maximumAttempts = 0;
        result.expectTrue(backend::detail::isAutomaticRecoveryEligible(
                              provider, options, Error{Error::Category::Transport, 5, "synthetic unlimited failure"}),
                          "maximumAttempts zero leaves classified recovery attempts unlimited");
    }

    typed::Thread retainedThread(std::string threadId, std::string turnId, bool active, std::string itemId, std::string text) {
        typed::AgentMessageThreadItem item;
        item.metadata = {typed::ItemId{std::move(itemId)}, typed::ThreadId{threadId}, typed::TurnId{turnId}, Json::object()};
        item.text = std::move(text);

        typed::Turn turn;
        turn.id = typed::TurnId{std::move(turnId)};
        turn.threadId = typed::ThreadId{threadId};
        turn.status = active ? typed::TurnStatus::inProgress() : typed::TurnStatus::completed();
        turn.items = {typed::ThreadItem{std::move(item)}};
        turn.raw = Json::object({{"items", Json::array({Json{{"id", "raw-duplicate"}}})}});

        typed::Thread thread;
        thread.id = typed::ThreadId{std::move(threadId)};
        thread.turns = {std::move(turn)};
        thread.raw = Json::object({{"turns", Json::array({Json{{"id", "raw-duplicate"}}})}});
        return thread;
    }

    struct RecomputedRetention {
        std::size_t threads = 0;
        std::size_t turns = 0;
        std::size_t items = 0;
        std::size_t contentBytes = 0;
    };

    RecomputedRetention recomputeRetention(const backend::BackendState& state) {
        RecomputedRetention result;
        result.threads = state.threads.size();
        for (const auto& [threadId, thread] : state.threads) {
            (void) threadId;
            result.turns += thread.turns.size();
            for (const auto& [turnId, turn] : thread.turns) {
                (void) turnId;
                result.items += turn.items.size();
                for (const auto& [itemId, item] : turn.items) {
                    (void) itemId;
                    result.contentBytes +=
                        item.agentText.size() + item.reasoningText.size() + item.reasoningSummary.size() + item.commandOutput.size();
                }
            }
        }
        return result;
    }

    bool retentionCountersMatch(const backend::BackendState& state) {
        const RecomputedRetention recomputed = recomputeRetention(state);
        return state.capacity.retainedThreads == recomputed.threads && state.capacity.retainedTurns == recomputed.turns &&
               state.capacity.retainedItems == recomputed.items && state.capacity.accumulatedContentBytes == recomputed.contentBytes;
    }

    void testTargetedItemSnapshotBatch(tests::support::TestResult& result) {
        backend::BackendState state;
        backend::Reducer reducer;
        reducer.apply(state,
                      backend::ThreadUpserted{retainedThread("thread-a", "turn-a", true, "shared-item", ""),
                                              backend::EntityLoad::Full});
        reducer.apply(state,
                      backend::ThreadUpserted{retainedThread("thread-b", "turn-b", true, "shared-item", ""),
                                              backend::EntityLoad::Full});
        reducer.apply(state,
                      backend::ItemContentChanged{typed::ThreadId{"thread-a"},
                                                  typed::TurnId{"turn-a"},
                                                  typed::ItemId{"shared-item"},
                                                  backend::ItemContentChanged::Kind::AgentText,
                                                  "alpha",
                                                  std::nullopt});
        reducer.apply(state,
                      backend::ItemContentChanged{typed::ThreadId{"thread-b"},
                                                  typed::TurnId{"turn-b"},
                                                  typed::ItemId{"shared-item"},
                                                  backend::ItemContentChanged::Kind::AgentText,
                                                  "beta",
                                                  std::nullopt});
        state.sequence = backend::SequenceNumber{17};

        const std::vector<backend::ItemSnapshotKey> keys{
            {typed::ThreadId{"thread-a"}, typed::TurnId{"turn-a"}, typed::ItemId{"shared-item"}},
            {typed::ThreadId{"thread-b"}, typed::TurnId{"turn-b"}, typed::ItemId{"shared-item"}},
        };
        const auto targeted = backend::makeItemSnapshotBatch(state, keys);
        const backend::Snapshot ordinary = backend::makeSnapshot(state);
        const backend::ItemSnapshot* ordinaryA = nullptr;
        const backend::ItemSnapshot* ordinaryB = nullptr;
        for (const backend::ThreadSnapshot& thread : ordinary.threads) {
            for (const backend::TurnSnapshot& turn : thread.turns) {
                for (const backend::ItemSnapshot& item : turn.items) {
                    if (thread.id == "thread-a" && turn.id == "turn-a") {
                        ordinaryA = &item;
                    } else if (thread.id == "thread-b" && turn.id == "turn-b") {
                        ordinaryB = &item;
                    }
                }
            }
        }
        result.expectTrue(targeted && targeted->sequence == backend::SequenceNumber{17} && targeted->items.size() == 2 &&
                              ordinaryA != nullptr && ordinaryB != nullptr && targeted->items[0] == *ordinaryA &&
                              targeted->items[1] == *ordinaryB && targeted->items[0].agentText == "alpha" &&
                              targeted->items[1].agentText == "beta",
                          "targeted item snapshots preserve exact composite identity and ordinary snapshot bounding");

        std::vector<backend::ItemSnapshotKey> withMissing = keys;
        withMissing.push_back(
            {typed::ThreadId{"thread-a"}, typed::TurnId{"turn-a"}, typed::ItemId{"missing-item"}});
        result.expectTrue(!backend::makeItemSnapshotBatch(state, withMissing),
                          "a missing exact item makes the targeted snapshot batch wholly unavailable");
    }

    void testReducerCapacityAndFreshness(tests::support::TestResult& result) {
        backend::BackendState state;
        backend::Reducer reducer;
        backend::ProviderState provider;
        provider.lifecycle = backend::ProviderLifecycle::Ready;
        provider.generation = 9;
        provider.desiredRunning = true;
        reducer.apply(state, backend::ProviderLifecycleChanged{provider});

        backend::BackendCapacityOptions limits;
        limits.maxRetainedThreads = 1;
        limits.maxRetainedTurns = 2;
        limits.maxRetainedItems = 2;
        limits.maxAccumulatedContentBytes = 4;
        limits.maxSnapshotBytes = 4096;
        reducer.apply(state, backend::CapacityConfigured{limits});

        reducer.apply(
            state, backend::ThreadUpserted{retainedThread("protected", "active", true, "active-item", "keep"), backend::EntityLoad::Full});
        const backend::Reduction retention = reducer.apply(
            state,
            backend::ThreadUpserted{retainedThread("optional", "terminal", false, "terminal-item", "drop"), backend::EntityLoad::Full});
        result.expectTrue(state.threads.size() == 1 && state.threads.contains("protected") && state.capacity.evictedThreads == 0 &&
                              state.capacity.snapshotOmissions == 1 && retentionCountersMatch(state),
                          "oldest-first thread capacity retains protected active state and omits the optional insertion");
        result.expectTrue(
            std::ranges::find(retention.capacityChanges, backend::CapacityChanged{backend::CapacityMetric::SnapshotOmissions, 1}) !=
                retention.capacityChanges.end(),
            "automatic retention accounting produces an explicit reducer-derived capacity event");
        const backend::ThreadState& protectedThread = state.threads.at("protected");
        result.expectTrue(protectedThread.stamp == backend::SourceStamp{9, backend::Freshness::Current} &&
                              protectedThread.turns.at("active").stamp.freshness == backend::Freshness::Current,
                          "current-generation results stamp retained thread and turn state Current");
        result.expectTrue(protectedThread.thread.turns.empty() && !protectedThread.thread.raw.contains("turns") &&
                              protectedThread.turns.at("active").turn.items.empty() &&
                              !protectedThread.turns.at("active").turn.raw.contains("items"),
                          "retained parent aggregates contain no duplicate nested turns, items, or raw arrays outside bounded maps");

        backend::BackendState evictionState;
        reducer.apply(evictionState, backend::ProviderLifecycleChanged{provider});
        reducer.apply(evictionState, backend::CapacityConfigured{limits});
        reducer.apply(evictionState,
                      backend::ThreadUpserted{retainedThread("oldest", "old-turn", false, "old-item", "old"), backend::EntityLoad::Full});
        const backend::Reduction eviction = reducer.apply(
            evictionState,
            backend::ThreadUpserted{retainedThread("newest", "new-turn", false, "new-item", "new"), backend::EntityLoad::Full});
        result.expectTrue(
            evictionState.threads.size() == 1 && evictionState.threads.contains("newest") && evictionState.capacity.evictedThreads == 1 &&
                evictionState.threadOrder.size() == 1 && evictionState.threadOrder.front().value == "newest" &&
                retentionCountersMatch(evictionState) &&
                std::ranges::find(eviction.capacityChanges, backend::CapacityChanged{backend::CapacityMetric::EvictedThreads, 1}) !=
                    eviction.capacityChanges.end(),
            "thread capacity evicts the oldest inactive thread without diverging its map and explicit order vector");

        backend::BackendState nestedThreadReferenceState;
        reducer.apply(nestedThreadReferenceState, backend::ProviderLifecycleChanged{provider});
        reducer.apply(nestedThreadReferenceState, backend::CapacityConfigured{limits});
        reducer.apply(nestedThreadReferenceState,
                      backend::ThreadUpserted{retainedThread("nested-protected", "turn", false, "item", "one"), backend::EntityLoad::Full});
        typed::ApplyPatchApprovalParams applyParams;
        applyParams.callId = typed::ResponseCallId{"apply"};
        applyParams.conversationId = typed::ThreadId{"nested-protected"};
        typed::ApplyPatchApprovalRequest applyRequest{ai::openai::codex::ServerRequestId{std::int64_t{2}},
                                                      ai::openai::codex::ServerRequestToken{2},
                                                      std::move(applyParams),
                                                      Json::object(),
                                                      {}};
        reducer.apply(nestedThreadReferenceState,
                      backend::PendingRequestAdded{backend::PendingRequestState{
                          backend::PendingRequestId{2}, typed::TypedServerRequest{std::move(applyRequest)}, 9}});
        reducer.apply(nestedThreadReferenceState,
                      backend::ThreadUpserted{retainedThread("apply-optional", "turn", false, "item", "two"), backend::EntityLoad::Full});
        result.expectTrue(nestedThreadReferenceState.threads.contains("nested-protected") &&
                              !nestedThreadReferenceState.threads.contains("apply-optional"),
                          "legacy apply-patch request metadata protects its nested conversation thread from eviction");
        reducer.apply(nestedThreadReferenceState, backend::PendingRequestRemoved{backend::PendingRequestId{2}, "test transition"});
        typed::ExecCommandApprovalParams execParams;
        execParams.callId = typed::ResponseCallId{"exec"};
        execParams.conversationId = typed::ThreadId{"nested-protected"};
        execParams.cwd = "/synthetic";
        typed::ExecCommandApprovalRequest execRequest{ai::openai::codex::ServerRequestId{std::int64_t{3}},
                                                      ai::openai::codex::ServerRequestToken{3},
                                                      std::move(execParams),
                                                      Json::object(),
                                                      {}};
        reducer.apply(nestedThreadReferenceState,
                      backend::PendingRequestAdded{backend::PendingRequestState{
                          backend::PendingRequestId{3}, typed::TypedServerRequest{std::move(execRequest)}, 9}});
        reducer.apply(nestedThreadReferenceState,
                      backend::ThreadUpserted{retainedThread("exec-optional", "turn", false, "item", "three"), backend::EntityLoad::Full});
        result.expectTrue(nestedThreadReferenceState.threads.contains("nested-protected") &&
                              !nestedThreadReferenceState.threads.contains("exec-optional"),
                          "legacy exec-command request metadata protects its nested conversation thread from eviction");

        reducer.apply(state, backend::ProviderConnectionInvalidated{9, "synthetic disconnect"});
        const backend::ThreadState& staleThread = state.threads.at("protected");
        result.expectTrue(staleThread.stamp.freshness == backend::Freshness::Stale &&
                              staleThread.turns.at("active").connectionInvalidated && !staleThread.turns.at("active").terminal,
                          "provider invalidation marks retained active state stale without fabricating a terminal outcome");

        backend::BackendState contentState;
        reducer.apply(contentState, backend::ProviderLifecycleChanged{provider});
        limits.maxRetainedThreads = 2;
        constexpr std::size_t SnapshotLimit = 3072;
        limits.maxSnapshotBytes = SnapshotLimit;
        reducer.apply(contentState, backend::CapacityConfigured{limits});
        reducer.apply(contentState,
                      backend::ThreadUpserted{retainedThread("old", "old-turn", false, "old-item", "abcd"), backend::EntityLoad::Full});
        reducer.apply(contentState,
                      backend::ThreadUpserted{retainedThread("new", "new-turn", false, "new-item", "efgh"), backend::EntityLoad::Full});
        const backend::ItemState& oldItem = contentState.threads.at("old").turns.at("old-turn").items.at("old-item");
        const backend::ItemState& newItem = contentState.threads.at("new").turns.at("new-turn").items.at("new-item");
        result.expectTrue(oldItem.agentText.empty() && newItem.agentText == "efgh" && oldItem.droppedContentBytes == 4 &&
                              contentState.capacity.droppedContentBytes == 4 && retentionCountersMatch(contentState),
                          "global content capacity trims the oldest inactive terminal item and preserves newest content");

        backend::BackendState turnState;
        reducer.apply(turnState, backend::ProviderLifecycleChanged{provider});
        limits.maxRetainedTurns = 1;
        limits.maxRetainedItems = 4;
        limits.maxAccumulatedContentBytes = 64;
        limits.maxSnapshotBytes = 8U * 1024U * 1024U;
        reducer.apply(turnState, backend::CapacityConfigured{limits});
        reducer.apply(
            turnState,
            backend::ThreadUpserted{retainedThread("first", "first-turn", false, "first-item", "one"), backend::EntityLoad::Full});
        reducer.apply(
            turnState,
            backend::ThreadUpserted{retainedThread("second", "second-turn", false, "second-item", "two"), backend::EntityLoad::Full});
        result.expectTrue(turnState.threads.at("first").turns.empty() && turnState.threads.at("second").turns.contains("second-turn") &&
                              turnState.threads.at("first").turnOrder.empty() &&
                              turnState.threads.at("second").turnOrder == std::vector<typed::TurnId>{typed::TurnId{"second-turn"}} &&
                              turnState.capacity.evictedTurns == 1 && retentionCountersMatch(turnState),
                          "global turn capacity evicts deterministically without diverging maps and order vectors");

        backend::BackendState itemState;
        reducer.apply(itemState, backend::ProviderLifecycleChanged{provider});
        limits.maxRetainedTurns = 2;
        limits.maxRetainedItems = 1;
        reducer.apply(itemState, backend::CapacityConfigured{limits});
        reducer.apply(
            itemState,
            backend::ThreadUpserted{retainedThread("items", "items-turn", false, "protected-item", "one"), backend::EntityLoad::Full});
        typed::CommandApprovalRequest request{ai::openai::codex::ServerRequestId{std::int64_t{1}},
                                              ai::openai::codex::ServerRequestToken{1},
                                              typed::ThreadId{"items"},
                                              typed::TurnId{"items-turn"},
                                              typed::ItemId{"protected-item"},
                                              0,
                                              std::nullopt,
                                              std::nullopt,
                                              std::nullopt,
                                              Json::object(),
                                              Json::object(),
                                              {},
                                              {}};
        reducer.apply(itemState,
                      backend::PendingRequestAdded{
                          backend::PendingRequestState{backend::PendingRequestId{1}, typed::TypedServerRequest{request}, 9}});
        typed::AgentMessageThreadItem optionalItem;
        optionalItem.metadata = {typed::ItemId{"optional-item"}, typed::ThreadId{"items"}, typed::TurnId{"items-turn"}, Json::object()};
        optionalItem.text = "two";
        reducer.apply(itemState,
                      backend::ItemUpserted{typed::ThreadId{"items"},
                                            typed::TurnId{"items-turn"},
                                            typed::ThreadItem{std::move(optionalItem)},
                                            backend::ItemLifecycle::Completed,
                                            std::nullopt});
        const backend::TurnState& retainedItems = itemState.threads.at("items").turns.at("items-turn");
        result.expectTrue(retainedItems.items.contains("protected-item") && !retainedItems.items.contains("optional-item") &&
                              itemState.capacity.evictedItems == 0 && itemState.capacity.snapshotOmissions == 1 &&
                              retentionCountersMatch(itemState),
                          "item capacity protects pending-request referenced state and omits the new optional alternative");

        reducer.apply(itemState, backend::PendingRequestRemoved{backend::PendingRequestId{1}, "test transition"});
        typed::PermissionsRequestApprovalParams permissionParams;
        permissionParams.cwd = typed::AbsolutePath{"/synthetic"};
        permissionParams.threadId = typed::ThreadId{"items"};
        permissionParams.turnId = typed::TurnId{"items-turn"};
        permissionParams.itemId = typed::ItemId{"protected-item"};
        typed::PermissionsApprovalRequest permissionRequest{ai::openai::codex::ServerRequestId{std::int64_t{4}},
                                                            ai::openai::codex::ServerRequestToken{4},
                                                            std::move(permissionParams),
                                                            Json::object(),
                                                            {}};
        reducer.apply(itemState,
                      backend::PendingRequestAdded{backend::PendingRequestState{
                          backend::PendingRequestId{4}, typed::TypedServerRequest{std::move(permissionRequest)}, 9}});
        typed::AgentMessageThreadItem anotherOptionalItem;
        anotherOptionalItem.metadata = {
            typed::ItemId{"another-optional"}, typed::ThreadId{"items"}, typed::TurnId{"items-turn"}, Json::object()};
        anotherOptionalItem.text = "three";
        reducer.apply(itemState,
                      backend::ItemUpserted{typed::ThreadId{"items"},
                                            typed::TurnId{"items-turn"},
                                            typed::ThreadItem{std::move(anotherOptionalItem)},
                                            backend::ItemLifecycle::Completed,
                                            std::nullopt});
        result.expectTrue(itemState.threads.at("items").turns.at("items-turn").items.contains("protected-item") &&
                              !itemState.threads.at("items").turns.at("items-turn").items.contains("another-optional"),
                          "permissions request metadata protects its nested thread, turn, and item from eviction");

        backend::BackendState activeTurnItems;
        reducer.apply(activeTurnItems, backend::ProviderLifecycleChanged{provider});
        reducer.apply(activeTurnItems, backend::CapacityConfigured{limits});
        reducer.apply(
            activeTurnItems,
            backend::ThreadUpserted{retainedThread("active-items", "active-turn", true, "old-complete", "one"), backend::EntityLoad::Full});
        typed::AgentMessageThreadItem completedOldItem;
        completedOldItem.metadata = {
            typed::ItemId{"old-complete"}, typed::ThreadId{"active-items"}, typed::TurnId{"active-turn"}, Json::object()};
        reducer.apply(activeTurnItems,
                      backend::ItemUpserted{typed::ThreadId{"active-items"},
                                            typed::TurnId{"active-turn"},
                                            typed::ThreadItem{std::move(completedOldItem)},
                                            backend::ItemLifecycle::Completed,
                                            std::nullopt});
        typed::AgentMessageThreadItem newestCompletedItem;
        newestCompletedItem.metadata = {
            typed::ItemId{"new-complete"}, typed::ThreadId{"active-items"}, typed::TurnId{"active-turn"}, Json::object()};
        reducer.apply(activeTurnItems,
                      backend::ItemUpserted{typed::ThreadId{"active-items"},
                                            typed::TurnId{"active-turn"},
                                            typed::ThreadItem{std::move(newestCompletedItem)},
                                            backend::ItemLifecycle::Completed,
                                            std::nullopt});
        const backend::TurnState& activeTurn = activeTurnItems.threads.at("active-items").turns.at("active-turn");
        result.expectTrue(activeTurn.active && !activeTurn.terminal && !activeTurn.items.contains("old-complete") &&
                              activeTurn.items.contains("new-complete") &&
                              activeTurn.itemOrder == std::vector<typed::ItemId>{typed::ItemId{"new-complete"}} &&
                              activeTurnItems.capacity.evictedItems == 1 && retentionCountersMatch(activeTurnItems),
                          "global item capacity evicts from an active turn without diverging its map and order vector");

        const backend::Snapshot pendingSnapshot = backend::makeSnapshot(itemState);
        result.expectTrue(pendingSnapshot.pendingRequests.size() == 1 && pendingSnapshot.pendingRequests.front().threadId == "items" &&
                              pendingSnapshot.pendingRequests.front().turnId == "items-turn" &&
                              pendingSnapshot.pendingRequests.front().itemId == "protected-item",
                          "safe pending-request snapshots retain nested reference metadata for protected-state projection");
        const backend::Reduction pendingInvalidation =
            reducer.apply(itemState, backend::ProviderConnectionInvalidated{9, "provider connection invalidated"});
        result.expectTrue(itemState.pendingRequests.empty() && pendingInvalidation.pendingRequestRemovals.size() == 1 &&
                              pendingInvalidation.pendingRequestRemovals.front().id == backend::PendingRequestId{4},
                          "provider invalidation clears canonical pending ownership and derives one ordered removal transition");

        backend::BackendState refreshState;
        backend::ProviderState firstGeneration = provider;
        firstGeneration.generation = 1;
        reducer.apply(refreshState, backend::ProviderLifecycleChanged{firstGeneration});
        typed::ThreadListResponse firstRefresh;
        firstRefresh.data = {retainedThread("confirmed", "turn", false, "item", "one"),
                             retainedThread("unconfirmed", "turn", false, "item", "two")};
        firstRefresh.nextCursor = std::string{"next"};
        reducer.apply(refreshState, backend::ThreadListUpdated{firstRefresh, std::nullopt, true});
        reducer.apply(refreshState,
                      backend::ThreadUpserted{retainedThread("confirmed", "turn", false, "item", "one"), backend::EntityLoad::Full});
        typed::ThreadListResponse laterPage;
        reducer.apply(refreshState, backend::ThreadListUpdated{laterPage, std::string{"next"}, false});
        const backend::Reduction invalidation =
            reducer.apply(refreshState, backend::ProviderConnectionInvalidated{1, "synthetic refresh boundary"});
        result.expectTrue(invalidation.pendingRequestRemovals.empty(),
                          "provider invalidation with no pending requests emits no synthetic removal");
        backend::ProviderState secondGeneration = provider;
        secondGeneration.generation = 2;
        reducer.apply(refreshState, backend::ProviderLifecycleChanged{secondGeneration});
        typed::ThreadListResponse secondRefresh;
        secondRefresh.data = {retainedThread("confirmed", "turn", false, "item", "new")};
        reducer.apply(refreshState, backend::ThreadListUpdated{secondRefresh, std::nullopt, true});
        result.expectTrue(
            refreshState.threadList.pagesLoaded == 1 &&
                refreshState.threadList.stamp == backend::SourceStamp{2, backend::Freshness::Current} &&
                refreshState.threads.at("confirmed").stamp == backend::SourceStamp{2, backend::Freshness::Current} &&
                !refreshState.threads.at("confirmed").fullyLoaded &&
                refreshState.threads.at("unconfirmed").stamp == backend::SourceStamp{1, backend::Freshness::Stale},
            "initial refresh resets generation-scoped pagination and load completeness and marks only confirmed cached entities Current");

        reducer.apply(refreshState, backend::ThreadListUpdated{laterPage, std::string{"same-generation"}, false});
        reducer.apply(refreshState, backend::ProviderConnectionInvalidated{2, "manual-list generation boundary"});
        backend::ProviderState thirdGeneration = provider;
        thirdGeneration.generation = 3;
        reducer.apply(refreshState, backend::ProviderLifecycleChanged{thirdGeneration});
        typed::ThreadListResponse manualFirstPage;
        manualFirstPage.data = {retainedThread("manual-confirmed", "turn", false, "item", "three")};
        reducer.apply(refreshState, backend::ThreadListUpdated{manualFirstPage, std::nullopt, false});
        result.expectTrue(refreshState.threadList.pagesLoaded == 1 &&
                              refreshState.threadList.stamp == backend::SourceStamp{3, backend::Freshness::Current},
                          "the first user-requested list page resets stale pagination when automatic hydration is disabled");
        typed::UnknownServerRequest currentRequest{ai::openai::codex::ServerRequestId{std::int64_t{5}},
                                                   ai::openai::codex::ServerRequestToken{5},
                                                   "future/current-request",
                                                   Json::object(),
                                                   Json::object(),
                                                   std::nullopt};
        reducer.apply(refreshState,
                      backend::PendingRequestAdded{backend::PendingRequestState{
                          backend::PendingRequestId{5}, typed::TypedServerRequest{std::move(currentRequest)}, 3}});
        const backend::Reduction staleInvalidation =
            reducer.apply(refreshState, backend::ProviderConnectionInvalidated{2, "stale provider callback"});
        result.expectTrue(!staleInvalidation.changed && staleInvalidation.pendingRequestRemovals.empty() &&
                              refreshState.pendingRequests.contains(backend::PendingRequestId{5}) &&
                              refreshState.threadList.stamp == backend::SourceStamp{3, backend::Freshness::Current},
                          "a stale-generation invalidation cannot clear current requests or mark current provider state stale");

        contentState.capacity.rejectedSessions = std::numeric_limits<std::uint64_t>::max();
        reducer.apply(contentState, backend::CapacityChanged{backend::CapacityMetric::RejectedSessions, 1});
        result.expectTrue(contentState.capacity.rejectedSessions == std::numeric_limits<std::uint64_t>::max(),
                          "capacity accounting saturates instead of wrapping");

        const backend::Snapshot bounded = backend::makeSnapshot(contentState);
        result.expectTrue(bounded.capacity.truncated &&
                              bounded.capacity.omittedThreads + bounded.capacity.omittedTurns + bounded.capacity.omittedItems != 0,
                          "snapshot construction deterministically omits oldest inactive state at its byte ceiling");
        result.expectTrue(bounded.provider.generation == 9 && bounded.capacity.state.limits.maxSnapshotBytes == SnapshotLimit,
                          "minimal bounded snapshots retain provider generation and configured capacity policy");
        result.expectTrue(!bounded.capacity.mandatoryCoreExceedsLimit && backend::snapshotSizeBytes(bounded) <= SnapshotLimit,
                          "a representable snapshot ceiling is enforced against the exact safe serialized projection");

        backend::BackendState optionalSnapshotState;
        optionalSnapshotState.provider = provider;
        optionalSnapshotState.provider.initialization =
            typed::InitializeResponse{typed::AbsolutePath{"/synthetic/codex"}, "linux", "linux", "aisuite-test", Json::object()};
        optionalSnapshotState.sessions.emplace(backend::SessionId{1},
                                               backend::ConnectedSessionState{backend::SessionId{1}, backend::SessionRole::Controller});
        optionalSnapshotState.controller = backend::SessionId{1};
        typed::UnknownServerRequest optionalPending{ai::openai::codex::ServerRequestId{std::int64_t{7}},
                                                    ai::openai::codex::ServerRequestToken{7},
                                                    "future/optional-snapshot",
                                                    Json::object(),
                                                    Json::object(),
                                                    std::nullopt};
        optionalSnapshotState.pendingRequests.emplace(
            backend::PendingRequestId{7},
            backend::PendingRequestState{backend::PendingRequestId{7}, typed::TypedServerRequest{std::move(optionalPending)}, 9});
        const std::size_t mandatorySnapshotBytes = backend::snapshotSizeBytes(backend::makeSnapshot(optionalSnapshotState));
        optionalSnapshotState.capacity.limits.maxSnapshotBytes = mandatorySnapshotBytes + 256;

        const std::string largeOptionalField(2048, 'o');
        optionalSnapshotState.providerOperations.emplace(
            "operation",
            backend::ProviderOperationState{
                largeOptionalField, backend::ProviderOperationValue{typed::Unit{}}.index(), {9, backend::Freshness::Current}});
        const typed::Event optionalEvent{typed::UnknownEvent{"future/optional", Json::object(), Json::object(), std::nullopt}};
        optionalSnapshotState.accounts.latestNotifications.emplace(
            largeOptionalField,
            backend::ProviderNotificationState{largeOptionalField, optionalEvent.index(), {9, backend::Freshness::Current}});
        optionalSnapshotState.notices.push_back(
            {1, backend::NoticeCategory::Warning, largeOptionalField, largeOptionalField, std::nullopt, {9, backend::Freshness::Current}});
        optionalSnapshotState.capacity.retainedNotices = 1;
        optionalSnapshotState.processes.emplace(
            largeOptionalField,
            backend::ProcessState{largeOptionalField, "exited", {}, {}, false, false, 0, 0, {9, backend::Freshness::Current}, false});
        optionalSnapshotState.processOrder.push_back(largeOptionalField);
        optionalSnapshotState.capacity.retainedProcesses = 1;
        optionalSnapshotState.filesystemWatches.emplace(
            largeOptionalField,
            backend::FilesystemWatchState{typed::FsWatchId{largeOptionalField}, std::nullopt, {}, {9, backend::Freshness::Stale}, true});
        optionalSnapshotState.filesystemWatchOrder.push_back(largeOptionalField);
        optionalSnapshotState.capacity.retainedFilesystemWatches = 1;
        optionalSnapshotState.fuzzySearchSessions.emplace(
            largeOptionalField, backend::FuzzySearchState{largeOptionalField, {}, {}, true, {9, backend::Freshness::Current}, false});
        optionalSnapshotState.fuzzySearchOrder.push_back(largeOptionalField);
        optionalSnapshotState.capacity.retainedFuzzySearchSessions = 1;
        backend::ActivityRecordState optionalActivity;
        optionalActivity.key = largeOptionalField;
        optionalActivity.subjectId = largeOptionalField;
        optionalActivity.kind = "test";
        optionalActivity.lifecycle = "completed";
        optionalActivity.notification = {"future/optional", optionalEvent.index(), {9, backend::Freshness::Current}};
        optionalSnapshotState.activities.emplace(largeOptionalField, std::move(optionalActivity));
        optionalSnapshotState.activityOrder.push_back(largeOptionalField);
        optionalSnapshotState.capacity.retainedActivityRecords = 1;

        const backend::Snapshot boundedOptional = backend::makeSnapshot(optionalSnapshotState);
        result.expectTrue(boundedOptional.capacity.truncated && !boundedOptional.capacity.mandatoryCoreExceedsLimit &&
                              backend::snapshotSizeBytes(boundedOptional) <= optionalSnapshotState.capacity.limits.maxSnapshotBytes &&
                              boundedOptional.provider.initialization && boundedOptional.providerOperations.empty() &&
                              boundedOptional.accounts.latestNotificationMethods.empty() && boundedOptional.notices.empty() &&
                              boundedOptional.processes.empty() && boundedOptional.filesystemWatches.empty() &&
                              boundedOptional.fuzzySearchSessions.empty() && boundedOptional.activities.empty(),
                          "snapshot bounding omits A1.6b optional domain summaries deterministically before using the minimal fallback");
        result.expectTrue(boundedOptional.controller == backend::SessionId{1} && boundedOptional.sessions.size() == 1 &&
                              boundedOptional.pendingRequests.size() == 1 &&
                              boundedOptional.pendingRequests.front().id == backend::PendingRequestId{7},
                          "optional A1.6b omissions preserve mandatory controller, session, and safe pending-request summaries");

        backend::BackendState zeroSnapshotState;
        backend::BackendCapacityOptions zeroSnapshotLimits;
        zeroSnapshotLimits.maxSnapshotBytes = 0;
        backend::ProviderState failedProvider;
        failedProvider.lifecycle = backend::ProviderLifecycle::Failed;
        failedProvider.generation = 11;
        failedProvider.desiredRunning = true;
        failedProvider.lastError = Error{Error::Category::Protocol, 91, "synthetic mandatory error"};
        reducer.apply(zeroSnapshotState, backend::ProviderLifecycleChanged{failedProvider});
        zeroSnapshotState.sessions.emplace(backend::SessionId{1},
                                           backend::ConnectedSessionState{backend::SessionId{1}, backend::SessionRole::Controller});
        zeroSnapshotState.controller = backend::SessionId{1};
        typed::UnknownServerRequest mandatoryRequest{ai::openai::codex::ServerRequestId{std::int64_t{6}},
                                                     ai::openai::codex::ServerRequestToken{6},
                                                     "future/mandatory-request",
                                                     Json::object(),
                                                     Json::object(),
                                                     std::nullopt};
        zeroSnapshotState.pendingRequests.emplace(
            backend::PendingRequestId{6},
            backend::PendingRequestState{backend::PendingRequestId{6}, typed::TypedServerRequest{std::move(mandatoryRequest)}, 11});
        reducer.apply(zeroSnapshotState, backend::CapacityConfigured{zeroSnapshotLimits});
        const backend::Snapshot zeroSnapshot = backend::makeSnapshot(zeroSnapshotState);
        result.expectTrue(zeroSnapshot.capacity.truncated && zeroSnapshot.capacity.mandatoryCoreExceedsLimit &&
                              zeroSnapshot.threads.empty() && zeroSnapshot.pendingRequests.size() == 1 &&
                              zeroSnapshot.pendingRequests.front().id == backend::PendingRequestId{6} &&
                              zeroSnapshot.pendingRequests.front().type == "unknown" &&
                              zeroSnapshot.pendingRequests.front().details.value("omitted", false) && zeroSnapshot.sessions.size() == 1 &&
                              zeroSnapshot.sessions.front().id == backend::SessionId{1},
                          "zero snapshot capacity emits only the mandatory valid envelope and explicitly reports its unavoidable size");
        result.expectTrue(zeroSnapshot.provider.lifecycle == backend::ProviderLifecycle::Failed && zeroSnapshot.provider.generation == 11 &&
                              zeroSnapshot.provider.lastError && zeroSnapshot.provider.lastError->category == "protocol" &&
                              zeroSnapshot.provider.lastError->code == 91 && zeroSnapshot.controller == backend::SessionId{1} &&
                              zeroSnapshot.capacity.sourceSessionCount == 1 && zeroSnapshot.capacity.sourcePendingRequestCount == 1,
                          "mandatory minimal snapshots preserve provider error, controller, session, and pending-request summaries");

        backend::BackendState malformedSnapshotState;
        reducer.apply(malformedSnapshotState, backend::ProviderLifecycleChanged{provider});
        backend::BackendCapacityOptions malformedLimits;
        reducer.apply(malformedSnapshotState, backend::CapacityConfigured{malformedLimits});
        typed::UserMessageThreadItem malformedItem;
        malformedItem.metadata = {typed::ItemId{"malformed-item"},
                                  typed::ThreadId{"malformed-thread"},
                                  typed::TurnId{"malformed-turn"},
                                  Json::object({{"content", Json::array({std::string(1, static_cast<char>(0xff))})}})};
        typed::Turn malformedTurn;
        malformedTurn.id = typed::TurnId{"malformed-turn"};
        malformedTurn.threadId = typed::ThreadId{"malformed-thread"};
        malformedTurn.status = typed::TurnStatus::completed();
        malformedTurn.items = {typed::ThreadItem{std::move(malformedItem)}};
        typed::Thread malformedThread;
        malformedThread.id = typed::ThreadId{"malformed-thread"};
        malformedThread.turns = {std::move(malformedTurn)};
        reducer.apply(malformedSnapshotState, backend::ThreadUpserted{std::move(malformedThread), backend::EntityLoad::Full});
        const backend::Snapshot malformedSnapshot = backend::makeSnapshot(malformedSnapshotState);
        result.expectTrue(malformedSnapshot.threads.size() == 1 && malformedSnapshot.threads.front().turns.size() == 1 &&
                              malformedSnapshot.threads.front().turns.front().items.size() == 1 &&
                              malformedSnapshot.threads.front().turns.front().items.front().data.value("omitted", false) &&
                              retentionCountersMatch(malformedSnapshotState),
                          "snapshot projection contains malformed serialized content without throwing or exposing its value");
    }

    void testIncrementalRetentionAndFreshness(tests::support::TestResult& result) {
        backend::ReducerOptions reducerOptions;
        reducerOptions.maxAccumulatedItemBytes = 4;
        backend::Reducer reducer(reducerOptions);
        backend::BackendState state;
        backend::ProviderState generationOne;
        generationOne.lifecycle = backend::ProviderLifecycle::Ready;
        generationOne.generation = 1;
        generationOne.desiredRunning = true;
        reducer.apply(state, backend::ProviderLifecycleChanged{generationOne});

        backend::BackendCapacityOptions limits;
        limits.maxRetainedThreads = 64;
        limits.maxRetainedTurns = 64;
        limits.maxRetainedItems = 64;
        limits.maxAccumulatedContentBytes = 4096;
        reducer.apply(state, backend::CapacityConfigured{limits});
        reducer.apply(state,
                      backend::ThreadUpserted{retainedThread("counter-thread", "counter-turn", true, "counter-item", "abcdef"),
                                              backend::EntityLoad::Full});
        result.expectTrue(retentionCountersMatch(state) && state.capacity.retainedThreads == 1 && state.capacity.retainedTurns == 1 &&
                              state.capacity.retainedItems == 1 && state.capacity.accumulatedContentBytes == 4,
                          "first insertion and per-item newest-suffix truncation update canonical retained-state counters");

        typed::AgentMessageThreadItem replacement;
        replacement.metadata = {
            typed::ItemId{"counter-item"}, typed::ThreadId{"counter-thread"}, typed::TurnId{"counter-turn"}, Json::object()};
        replacement.text = "xy";
        reducer.apply(state,
                      backend::ItemUpserted{typed::ThreadId{"counter-thread"},
                                            typed::TurnId{"counter-turn"},
                                            typed::ThreadItem{std::move(replacement)},
                                            backend::ItemLifecycle::Completed,
                                            std::nullopt});
        result.expectTrue(retentionCountersMatch(state) && state.capacity.retainedItems == 1 && state.capacity.accumulatedContentBytes == 2,
                          "replacement of an existing item adjusts content without double-counting retained entities");

        reducer.apply(state,
                      backend::ItemContentChanged{typed::ThreadId{"counter-thread"},
                                                  typed::TurnId{"counter-turn"},
                                                  typed::ItemId{"counter-item"},
                                                  backend::ItemContentChanged::Kind::AgentText,
                                                  "12345",
                                                  std::nullopt});
        result.expectTrue(retentionCountersMatch(state) && state.capacity.accumulatedContentBytes == 4 &&
                              state.threads.at("counter-thread").turns.at("counter-turn").items.at("counter-item").agentText == "2345",
                          "item delta append and newest-suffix replacement preserve exact content accounting");

        const backend::CapacityState beforeTransientChanges = state.capacity;
        typed::UnknownServerRequest request{ai::openai::codex::ServerRequestId{std::int64_t{10}},
                                            ai::openai::codex::ServerRequestToken{10},
                                            "future/counter-request",
                                            Json::object(),
                                            Json::object(),
                                            std::nullopt};
        reducer.apply(state,
                      backend::PendingRequestAdded{
                          backend::PendingRequestState{backend::PendingRequestId{10}, typed::TypedServerRequest{std::move(request)}, 1}});
        reducer.apply(state, backend::PendingRequestRemoved{backend::PendingRequestId{10}, "counter test"});
        const backend::Snapshot snapshot = backend::makeSnapshot(state);
        (void) snapshot;
        result.expectTrue(retentionCountersMatch(state) && state.capacity.retainedThreads == beforeTransientChanges.retainedThreads &&
                              state.capacity.retainedTurns == beforeTransientChanges.retainedTurns &&
                              state.capacity.retainedItems == beforeTransientChanges.retainedItems &&
                              state.capacity.accumulatedContentBytes == beforeTransientChanges.accumulatedContentBytes,
                          "pending-request changes and snapshot construction do not mutate retained-state counters");

        backend::BackendState deferredRequestState;
        reducer.apply(deferredRequestState, backend::ProviderLifecycleChanged{generationOne});
        typed::AttestationGenerateRequest deferredRequest{ai::openai::codex::ServerRequestId{std::string{"deferred"}},
                                                          ai::openai::codex::ServerRequestToken{11},
                                                          typed::AttestationGenerateParams{},
                                                          Json::object(),
                                                          {}};
        reducer.apply(deferredRequestState,
                      backend::PendingRequestAdded{backend::PendingRequestState{
                          backend::PendingRequestId{11}, typed::TypedServerRequest{std::move(deferredRequest)}, 1}});
        const backend::Reduction deferredInvalidation =
            reducer.apply(deferredRequestState, backend::ProviderConnectionInvalidated{1, "deferred occurrence invalidated"});
        result.expectTrue(deferredRequestState.pendingRequests.empty() && deferredInvalidation.pendingRequestRemovals.size() == 1 &&
                              deferredInvalidation.pendingRequestRemovals.front().id == backend::PendingRequestId{11},
                          "provider invalidation removes one retained A1.6b-deferred occurrence exactly once");

        backend::BackendState freshness;
        reducer.apply(freshness, backend::ProviderLifecycleChanged{generationOne});
        reducer.apply(freshness, backend::CapacityConfigured{limits});
        reducer.apply(
            freshness,
            backend::ThreadUpserted{retainedThread("fresh-thread", "fresh-turn", true, "fresh-item", "one"), backend::EntityLoad::Full});
        reducer.apply(freshness, backend::ProviderConnectionInvalidated{1, "generation boundary"});
        const RecomputedRetention invalidatedCounts = recomputeRetention(freshness);
        backend::ProviderState generationTwo = generationOne;
        generationTwo.generation = 2;
        reducer.apply(freshness, backend::ProviderLifecycleChanged{generationTwo});
        reducer.apply(freshness,
                      backend::ItemContentChanged{typed::ThreadId{"fresh-thread"},
                                                  typed::TurnId{"fresh-turn"},
                                                  typed::ItemId{"fresh-item"},
                                                  backend::ItemContentChanged::Kind::AgentText,
                                                  "two",
                                                  std::nullopt});
        const backend::ThreadState& afterItem = freshness.threads.at("fresh-thread");
        const backend::TurnState& afterItemTurn = afterItem.turns.at("fresh-turn");
        const backend::ItemState& afterItemState = afterItemTurn.items.at("fresh-item");
        result.expectTrue(afterItemState.stamp == backend::SourceStamp{2, backend::Freshness::Current} &&
                              !afterItemState.connectionInvalidated &&
                              afterItemTurn.stamp == backend::SourceStamp{1, backend::Freshness::Stale} &&
                              afterItemTurn.connectionInvalidated && afterItem.stamp == backend::SourceStamp{1, backend::Freshness::Stale},
                          "a generation-two item delta confirms only the item and preserves stale parent thread and turn metadata");

        reducer.apply(
            freshness,
            backend::TokenUsageUpdated{typed::ThreadId{"fresh-thread"}, typed::TurnId{"fresh-turn"}, Json::object({{"used", 1}})});
        const backend::ThreadState& afterTurn = freshness.threads.at("fresh-thread");
        result.expectTrue(afterTurn.turns.at("fresh-turn").stamp == backend::SourceStamp{2, backend::Freshness::Current} &&
                              !afterTurn.turns.at("fresh-turn").connectionInvalidated &&
                              afterTurn.stamp == backend::SourceStamp{1, backend::Freshness::Stale},
                          "an authoritative turn event confirms the turn without promoting stale parent thread metadata");
        reducer.apply(freshness,
                      backend::ThreadStatusUpdated{typed::ThreadId{"fresh-thread"}, typed::ThreadStatus{typed::IdleThreadStatus{}}});
        result.expectTrue(freshness.threads.at("fresh-thread").stamp == backend::SourceStamp{2, backend::Freshness::Current},
                          "an authoritative thread event independently confirms the parent thread");

        reducer.apply(freshness,
                      backend::ItemContentChanged{typed::ThreadId{"placeholder-thread"},
                                                  typed::TurnId{"placeholder-turn"},
                                                  typed::ItemId{"placeholder-item"},
                                                  backend::ItemContentChanged::Kind::AgentText,
                                                  "x",
                                                  std::nullopt});
        const backend::ThreadState& placeholder = freshness.threads.at("placeholder-thread");
        result.expectTrue(placeholder.stamp == backend::SourceStamp{2, backend::Freshness::Current} &&
                              placeholder.thread.raw.value("backendPlaceholder", false) &&
                              placeholder.turns.at("placeholder-turn").stamp == backend::SourceStamp{2, backend::Freshness::Current} &&
                              placeholder.turns.at("placeholder-turn").turn.raw.value("backendPlaceholder", false),
                          "a child event creates explicitly marked current-generation parent placeholders without stale metadata");
        result.expectTrue(retentionCountersMatch(freshness) && recomputeRetention(freshness).threads == invalidatedCounts.threads + 1,
                          "invalidation and entity-level reconfirmation preserve exact incremental retained-state counts");

        backend::BackendState fastPath;
        reducer.apply(fastPath, backend::ProviderLifecycleChanged{generationTwo});
        reducer.apply(fastPath, backend::CapacityConfigured{limits});
        for (std::size_t index = 0; index < 32; ++index) {
            const std::string suffix = std::to_string(index);
            reducer.apply(
                fastPath,
                backend::ThreadUpserted{retainedThread("fast-thread-" + suffix, "fast-turn-" + suffix, false, "fast-item-" + suffix, "x"),
                                        backend::EntityLoad::Full});
        }
        backend::detail::resetRetentionCapacityInstrumentation();
        for (std::size_t index = 0; index < 128; ++index) {
            reducer.apply(fastPath,
                          backend::ItemContentChanged{typed::ThreadId{"fast-thread-0"},
                                                      typed::TurnId{"fast-turn-0"},
                                                      typed::ItemId{"fast-item-0"},
                                                      backend::ItemContentChanged::Kind::AgentText,
                                                      "x",
                                                      std::nullopt});
        }
        const backend::detail::RetentionCapacityInstrumentation instrumentation = backend::detail::retentionCapacityInstrumentation();
        result.expectTrue(instrumentation.slowPathEntries == 0 && instrumentation.pendingReferenceBuilds == 0 &&
                              retentionCountersMatch(fastPath),
                          "under-limit item deltas use the O(1) retention fast path without eviction or pending-reference scans");
    }

    void testZeroHandleCapacities(tests::support::TestResult& result) {
        auto transport = std::make_shared<tests::codex::FakeTransportState>();
        backend::BackendCoreOptions options;
        options.capacity.maxSessions = 0;
        options.capacity.maxObservers = 0;
        FakeBackendCore backendCore(std::move(options), transport);

        backend::FrontendSession session = backendCore.openSession({});
        backend::BackendObserverSubscription observer = backendCore.subscribe({});
        const backend::Snapshot snapshot = backendCore.snapshot();
        result.expectTrue(!session.isOpen() && !observer.isOpen() && snapshot.sessions.empty() &&
                              snapshot.capacity.state.rejectedSessions == 1 && snapshot.capacity.state.rejectedObservers == 1,
                          "zero session and observer capacities reject only the new handles and account both boundaries");
        const backend::SequenceNumber stoppedSequence = backendCore.state().sequence;
        backendCore.stop();
        backendCore.stop();
        result.expectTrue(backendCore.state().sequence == stoppedSequence,
                          "repeated stop calls on an already stopped provider are canonical-state no-ops");
    }

    void testProviderStartRejectedDuringShutdown(tests::support::TestResult& result) {
        result.expectTrue(core::SNodeC::state() == core::State::STOPPING,
                          "provider shutdown-admission regression uses the real SNode.C STOPPING state");
        auto transport = std::make_shared<tests::codex::FakeTransportState>();
        tests::codex::FakeAppServerClient* client = nullptr;
        std::size_t recoverySchedules = 0;
        backend::BackendCoreOptions options;
        options.initialThreadListLimit = 1;
        options.recovery.enabled = true;
        options.recoveryTimerScheduler = [&recoverySchedules](std::uint64_t, std::function<void()>) {
            ++recoverySchedules;
            return backend::RecoveryTimerCancellation{};
        };
        FakeBackendCore backendCore(std::move(options), transport, &client);
        backendCore.start();
        const backend::Snapshot rejected = backendCore.snapshot();
        result.expectTrue(client != nullptr && client->getState() == ai::openai::codex::State::Stopped && transport->startCount == 0 &&
                              transport->callbackGenerations.empty(),
                          "SNode.C shutdown rejects provider transport startup before callbacks or process admission");
        result.expectTrue(rejected.provider.generation == 0 && rejected.provider.lifecycle == backend::ProviderLifecycle::Stopped &&
                              rejected.provider.desiredRunning && rejected.provider.recovery.status == backend::RecoveryStatus::Idle &&
                              recoverySchedules == 0,
                          "a shutdown-rejected provider start changes only requested running intent, not generation or lifecycle state");
        backendCore.stop();
        const backend::Snapshot stopped = backendCore.snapshot();
        const backend::SequenceNumber stoppedSequence = stopped.sequence;
        backendCore.stop();
        const backend::Snapshot stoppedAgain = backendCore.snapshot();
        result.expectTrue(stopped.provider.generation == 0 && stopped.provider.lifecycle == backend::ProviderLifecycle::Stopped &&
                              !stopped.provider.desiredRunning && stoppedAgain.sequence == stoppedSequence && recoverySchedules == 0,
                          "stop remains coherent and idempotent after shutdown rejected the provider start");
    }

    class ManualRecoveryScheduler {
    public:
        struct Entry {
            std::uint64_t delayMs = 0;
            std::function<void()> callback;
            std::shared_ptr<bool> active;
        };

        backend::RecoveryTimerCancellation schedule(std::uint64_t delayMs, std::function<void()> callback) {
            auto active = std::make_shared<bool>(true);
            entries.push_back({delayMs, std::move(callback), active});
            return [active]() {
                *active = false;
            };
        }

        void fire(std::size_t index, bool ignoreCancellation = false) {
            if (index >= entries.size()) {
                return;
            }
            Entry& entry = entries[index];
            if (!ignoreCancellation && !*entry.active) {
                return;
            }
            *entry.active = false;
            entry.callback();
        }

        std::vector<Entry> entries;
    };

    class FoundationRunner {
    public:
        explicit FoundationRunner(tests::support::TestResult& result)
            : result(result) {
        }

        void start() {
            configureTransport();

            backend::BackendCoreOptions options;
            options.initialThreadListLimit = 1;
            options.recovery = {true, 2, 10, 15, 2};
            options.capacity.maxSessions = 1;
            options.capacity.maxObservers = 1;
            options.capacity.maxActiveOperations = 1;
            options.capacity.maxPendingRequests = 1;
            options.recoveryTimerScheduler = [this](std::uint64_t delayMs, std::function<void()> callback) {
                return scheduler.schedule(delayMs, std::move(callback));
            };
            backendCore = std::make_unique<FakeBackendCore>(std::move(options), transport);
            backend::FrontendSessionCallbacks callbacks;
            callbacks.onCommandCompleted = [this](const backend::CommandCompletion& completion) {
                if (completion.requestId == "capacity-operation") {
                    capacityOperation = completion;
                } else if (completion.requestId == "post-hydration-operation") {
                    postHydrationOperation = completion;
                }
            };
            session = backendCore->openSession(std::move(callbacks));
            session.submit("controller", backend::ControllerAcquire{});
            const backend::Snapshot beforeStart = backendCore->snapshot();
            expect(beforeStart.provider.generation == 0 && beforeStart.sessions.size() == 1 && beforeStart.controller == session.id(),
                   "provider generation starts at zero and session/controller activity does not increment it");
            backendCore->start();
            const backend::Snapshot firstAttempt = backendCore->snapshot();
            expect(firstAttempt.provider.generation == 1 && transport->startCount == 0,
                   "the first accepted provider start creates generation one before the event-loop transport start");
            backendCore->restart();
            backendCore->restart();
            waitUntil(
                "the replacement provider generation becomes Ready with one bounded hydration request pending",
                [this]() {
                    const backend::Snapshot snapshot = backendCore->snapshot();
                    return backendCore->isReady() && pendingHydrationId.has_value() && snapshot.threadList.pagesLoaded == 0;
                },
                [this]() {
                    verifyInitialGeneration();
                });
        }

        bool finished() const noexcept {
            return isFinished;
        }

    private:
        void configureTransport() {
            transport = std::make_shared<tests::codex::FakeTransportState>();
            transport->sendHook = [this](const Json& message, const TransportCallbacks& callbacks) {
                const auto method = message.find("method");
                const auto id = message.find("id");
                if (method != message.end() && method->is_string() && *method == "initialize" && id != message.end() &&
                    respondToInitialize) {
                    if (duplicateRestartDuringReplacementInitialization) {
                        duplicateRestartDuringReplacementInitialization = false;
                        const std::uint64_t generation = backendCore->snapshot().provider.generation;
                        backendCore->restart();
                        backendCore->restart();
                        expect(backendCore->snapshot().provider.generation == generation,
                               "duplicate restart calls during replacement initialization are idempotent");
                    }
                    tests::codex::inject(callbacks, Json{{"id", *id}, {"result", tests::codex::initializeResult()}});
                    return;
                }
                if (method != message.end() && method->is_string() && *method == "thread/list" && id != message.end()) {
                    if (holdHydration && !pendingHydrationId) {
                        pendingHydrationId = *id;
                        pendingHydrationCallbacks = callbacks;
                        return;
                    }
                    tests::codex::inject(
                        callbacks,
                        Json{{"id", *id}, {"result", {{"data", Json::array()}, {"nextCursor", nullptr}, {"backwardsCursor", nullptr}}}});
                }
            };
        }

        static void defer(std::function<void()> callback) {
            core::EventReceiver::atNextTick(std::move(callback));
        }

        void expect(bool condition, const std::string& message) {
            result.expectTrue(condition, message);
        }

        void afterTicks(std::size_t ticks, std::function<void()> callback) {
            if (ticks == 0) {
                callback();
                return;
            }
            defer([this, ticks, callback = std::move(callback)]() mutable {
                afterTicks(ticks - 1, std::move(callback));
            });
        }

        void waitUntil(std::string description, std::function<bool()> predicate, std::function<void()> next, std::size_t remaining = 4000) {
            defer([this,
                   description = std::move(description),
                   predicate = std::move(predicate),
                   next = std::move(next),
                   remaining]() mutable {
                if (isFinished) {
                    return;
                }
                if (predicate()) {
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

        void verifyInitialGeneration() {
            const backend::Snapshot snapshot = backendCore->snapshot();
            expect(snapshot.provider.generation == 2 && transport->startCount == 1 && snapshot.threadList.pagesLoaded == 0 &&
                       snapshot.provider.recovery.status == backend::RecoveryStatus::Idle,
                   "restart from Starting waits for Stopped and admits exactly one bounded hydration operation");
            expect(session.isOpen() && session.role() == backend::SessionRole::Controller,
                   "the frontend session owns the controller independently of provider startup");
            const backend::FrontendSession rejectedSession = backendCore->openSession({});
            observer = backendCore->subscribe({});
            const backend::BackendObserverSubscription rejectedObserver = backendCore->subscribe({});
            expect(!rejectedSession.isOpen() && observer.isOpen() && !rejectedObserver.isOpen(),
                   "session and observer admission enforce their exact configured boundaries");

            backend::ThreadStart blocked;
            blocked.params.cwd = std::string{"/synthetic/capacity"};
            expect(static_cast<bool>(session.submit("capacity-operation", std::move(blocked))),
                   "an operation rejected by backend capacity still enters the asynchronous command lifecycle");
            waitUntil(
                "active-operation capacity completes asynchronously",
                [this]() {
                    return capacityOperation.has_value();
                },
                [this]() {
                    expect(capacityOperation->result.error &&
                               capacityOperation->result.error->code == backend::CommandErrorCode::LocalSubmissionFailure,
                           "active-operation capacity uses the existing local submission failure contract");
                    expect(std::none_of(transport->outgoing.begin(),
                                        transport->outgoing.end(),
                                        [](const Json& message) {
                                            return message.value("method", "") == "thread/start";
                                        }),
                           "capacity rejection emits no provider request");
                    const backend::CapacityState capacity = backendCore->snapshot().capacity.state;
                    expect(capacity.rejectedSessions == 1 && capacity.rejectedObservers == 1 && capacity.rejectedOperations == 1,
                           "session, observer, and global provider-operation rejections are reducer-visible and counted");
                    expect(pendingHydrationId.has_value(),
                           "the admitted hydration retains the sole global provider-operation slot while pending");
                    releaseHydration();
                    waitUntil(
                        "hydration completion releases the global provider-operation slot",
                        [this]() {
                            return backendCore->snapshot().threadList.pagesLoaded == 1;
                        },
                        [this]() {
                            backend::ThreadList command;
                            expect(static_cast<bool>(session.submit("post-hydration-operation", std::move(command))),
                                   "a later frontend operation enters the command lifecycle after hydration releases capacity");
                            waitUntil(
                                "the later frontend operation is admitted after hydration",
                                [this]() {
                                    return postHydrationOperation.has_value();
                                },
                                [this]() {
                                    expect(static_cast<bool>(postHydrationOperation->result),
                                           "a released hydration slot admits and completes the later provider operation");
                                    verifyRestartFromReady();
                                });
                        });
                });
        }

        void releaseHydration() {
            if (!pendingHydrationId) {
                return;
            }
            const Json id = *pendingHydrationId;
            const TransportCallbacks callbacks = pendingHydrationCallbacks;
            pendingHydrationId.reset();
            pendingHydrationCallbacks = {};
            holdHydration = false;
            tests::codex::inject(
                callbacks, Json{{"id", id}, {"result", {{"data", Json::array()}, {"nextCursor", nullptr}, {"backwardsCursor", nullptr}}}});
        }

        void verifyRestartFromReady() {
            const backend::Snapshot before = backendCore->snapshot();
            const std::size_t starts = transport->startCount;
            backendCore->restart();
            waitUntil(
                "manual restart from Ready completes one replacement generation",
                [this, generation = before.provider.generation, starts]() {
                    const backend::Snapshot snapshot = backendCore->snapshot();
                    return backendCore->isReady() && snapshot.provider.generation == generation + 1 &&
                           snapshot.threadList.pagesLoaded == 1 &&
                           snapshot.threadList.stamp == backend::SourceStamp{generation + 1, backend::Freshness::Current} &&
                           transport->startCount == starts + 1;
                },
                [this]() {
                    verifyRestartFromStopping();
                });
        }

        void verifyRestartFromStopping() {
            const backend::Snapshot before = backendCore->snapshot();
            const std::size_t starts = transport->startCount;
            transport->deferStopCompletion = true;
            backendCore->stop();
            backendCore->restart();
            backendCore->restart();
            expect(transport->startCount == starts && backendCore->snapshot().provider.generation == before.provider.generation,
                   "restart during an underlying Stopping transition waits without creating an overlapping provider");
            transport->deferStopCompletion = false;
            transport->completeDeferredStop();
            waitUntil(
                "manual restart requested during Stopping starts once after Stopped",
                [this, generation = before.provider.generation, starts]() {
                    const backend::Snapshot snapshot = backendCore->snapshot();
                    return backendCore->isReady() && snapshot.provider.generation == generation + 1 &&
                           snapshot.threadList.pagesLoaded == 1 &&
                           snapshot.threadList.stamp == backend::SourceStamp{generation + 1, backend::Freshness::Current} &&
                           transport->startCount == starts + 1;
                },
                [this]() {
                    verifyStopWinsQueuedFailure();
                });
        }

        void verifyStopWinsQueuedFailure() {
            const backend::Snapshot before = backendCore->snapshot();
            const std::size_t starts = transport->startCount;
            const std::size_t timers = scheduler.entries.size();
            transport->callbacks.onError(Error{Error::Category::Transport, 79, "synthetic queued failure"});
            backendCore->stop();
            waitUntil(
                "explicit stop wins over an already queued provider Failed callback",
                [this]() {
                    const backend::Snapshot snapshot = backendCore->snapshot();
                    return snapshot.provider.lifecycle == backend::ProviderLifecycle::Stopped && !snapshot.provider.desiredRunning;
                },
                [this, generation = before.provider.generation, starts, timers]() {
                    const backend::Snapshot stopped = backendCore->snapshot();
                    expect(stopped.provider.generation == generation && transport->startCount == starts &&
                               scheduler.entries.size() == timers,
                           "a queued failure cannot restore Failed or schedule recovery after an explicit stop");
                    verifyRestartFromInitializing();
                });
        }

        void verifyRestartFromInitializing() {
            const backend::Snapshot stopped = backendCore->snapshot();
            const std::size_t starts = transport->startCount;
            respondToInitialize = false;
            backendCore->start();
            waitUntil(
                "an ordinary provider start reaches Initializing",
                [this, generation = stopped.provider.generation]() {
                    const backend::Snapshot snapshot = backendCore->snapshot();
                    return snapshot.provider.lifecycle == backend::ProviderLifecycle::Initializing &&
                           snapshot.provider.generation == generation + 1;
                },
                [this, generation = stopped.provider.generation, starts]() {
                    backendCore->restart();
                    backendCore->restart();
                    respondToInitialize = true;
                    waitUntil(
                        "restart from Initializing replaces the provider exactly once",
                        [this, generation, starts]() {
                            const backend::Snapshot snapshot = backendCore->snapshot();
                            return backendCore->isReady() && snapshot.provider.generation == generation + 2 &&
                                   snapshot.threadList.pagesLoaded == 1 &&
                                   snapshot.threadList.stamp == backend::SourceStamp{generation + 2, backend::Freshness::Current} &&
                                   transport->startCount == starts + 2;
                        },
                        [this]() {
                            verifyIneligibleFailure(0);
                        });
                });
        }

        void verifyIneligibleFailure(std::size_t index) {
            static constexpr std::array<Error::Category, 7> Categories{Error::Category::Launch,
                                                                       Error::Category::Protocol,
                                                                       Error::Category::Initialization,
                                                                       Error::Category::InvalidState,
                                                                       Error::Category::Capacity,
                                                                       Error::Category::Cancelled,
                                                                       Error::Category::Enqueue};
            if (index == Categories.size()) {
                eligibleBaseGeneration = backendCore->snapshot().provider.generation;
                eligibleBaseStartCount = transport->startCount;
                createCurrentProviderState();
                return;
            }
            const std::size_t timersBefore = scheduler.entries.size();
            transport->callbacks.onError(Error{Categories[index], static_cast<int>(80U + index), "ineligible recovery failure"});
            waitUntil(
                "ineligible provider failure settles without automatic recovery",
                [this]() {
                    return backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Failed;
                },
                [this, index, timersBefore]() {
                    expect(scheduler.entries.size() == timersBefore,
                           "all non-Transport/non-Process failure categories avoid automatic recovery");
                    backendCore->restart();
                    waitUntil(
                        "manual restart recovers an ineligible provider failure",
                        [this]() {
                            return backendCore->isReady();
                        },
                        [this, index]() {
                            verifyIneligibleFailure(index + 1);
                        });
                });
        }

        void createCurrentProviderState() {
            transport->inject({{"method", "thread/started"}, {"params", {{"thread", tests::codex::threadValue("cached")}}}});
            transport->inject(
                {{"method", "turn/started"}, {"params", {{"threadId", "cached"}, {"turn", tests::codex::turnValue("cached", "active")}}}});
            waitUntil(
                "provider event creates current generation state",
                [this]() {
                    const backend::Snapshot current = backendCore->snapshot();
                    return !current.threads.empty() && !current.threads.front().turns.empty();
                },
                [this]() {
                    failForAutomaticRecovery();
                });
        }

        void failForAutomaticRecovery() {
            transport->callbacks.onError(Error{Error::Category::Transport, 71, "synthetic transport failure"});
            waitUntil(
                "eligible failure schedules the first deterministic retry",
                [this]() {
                    return scheduler.entries.size() == 1 &&
                           backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Recovering;
                },
                [this]() {
                    const backend::Snapshot waiting = backendCore->snapshot();
                    expect(waiting.provider.recovery.attempts == 1 && waiting.provider.recovery.delayMs == 10 &&
                               scheduler.entries[0].delayMs == 10,
                           "attempt one uses initialDelayMs through the injected event-loop timer seam");
                    expect(waiting.threads.front().stamp.freshness == backend::Freshness::Stale &&
                               waiting.threads.front().turns.front().connectionInvalidated,
                           "provider invalidation retains cached state as stale and marks active work connection-invalidated");
                    expect(session.isOpen() && session.role() == backend::SessionRole::Controller,
                           "automatic recovery retains sessions and controller ownership");
                    respondToInitialize = false;
                    const std::uint64_t generationBeforeTimer = waiting.provider.generation;
                    const std::size_t startsBeforeTimer = transport->startCount;
                    scheduler.fire(0);
                    expect(backendCore->snapshot().provider.generation == generationBeforeTimer &&
                               transport->startCount == startsBeforeTimer,
                           "even a synchronously firing injected timer cannot start the provider inline");
                    waitUntil(
                        "first recovery timer creates exactly one new provider generation",
                        [this]() {
                            return backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Initializing;
                        },
                        [this]() {
                            failSecondAttempt();
                        });
                });
        }

        void failSecondAttempt() {
            const backend::Snapshot initializing = backendCore->snapshot();
            expect(initializing.provider.generation == eligibleBaseGeneration + 1 && transport->startCount == eligibleBaseStartCount + 1,
                   "the recovery timer increments generation once for its actual provider start");
            transport->callbacks.onError(Error{Error::Category::Process, 72, "synthetic process failure"});
            waitUntil(
                "consecutive eligible failure schedules capped exponential retry",
                [this]() {
                    return scheduler.entries.size() == 2 &&
                           backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Recovering;
                },
                [this]() {
                    const backend::Snapshot waiting = backendCore->snapshot();
                    expect(waiting.provider.recovery.attempts == 2 && waiting.provider.recovery.delayMs == 15 &&
                               scheduler.entries[1].delayMs == 15,
                           "attempt two saturates deterministic exponential backoff at maximumDelayMs");
                    respondToInitialize = true;
                    scheduler.fire(1);
                    waitUntil(
                        "successful recovery resets attempts and records initialization",
                        [this]() {
                            return backendCore->isReady() && backendCore->snapshot().provider.generation == eligibleBaseGeneration + 2;
                        },
                        [this]() {
                            verifyRecoveredGeneration();
                        });
                });
        }

        void verifyRecoveredGeneration() {
            const backend::Snapshot ready = backendCore->snapshot();
            expect(ready.provider.recovery.status == backend::RecoveryStatus::Idle && ready.provider.recovery.attempts == 0 &&
                       !ready.provider.recovery.delayMs && !ready.provider.lastError && ready.provider.initialization.has_value(),
                   "Ready resets recovery bookkeeping and records safe initialization metadata");
            expect(ready.threads.front().stamp.freshness == backend::Freshness::Stale,
                   "reconnect alone does not mark unconfirmed cached entities Current");

            transport->callbacks.onError(Error{Error::Category::Transport, 73, "manual override failure"});
            waitUntil(
                "manual override has one pending recovery timer",
                [this]() {
                    return scheduler.entries.size() == 3 &&
                           backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Recovering;
                },
                [this]() {
                    const std::uint64_t generation = backendCore->snapshot().provider.generation;
                    backendCore->restart();
                    waitUntil(
                        "manual restart supersedes recovery and starts once",
                        [this, generation]() {
                            return backendCore->isReady() && backendCore->snapshot().provider.generation == generation + 1;
                        },
                        [this, generation]() {
                            const std::size_t starts = transport->startCount;
                            scheduler.fire(2, true);
                            defer([this, generation, starts]() {
                                expect(transport->startCount == starts && backendCore->snapshot().provider.generation == generation + 1,
                                       "a stale cancelled recovery callback cannot start another provider generation");
                                verifyFiniteAttemptExhaustion();
                            });
                        });
                });
        }

        void verifyFiniteAttemptExhaustion() {
            transport->callbacks.onError(Error{Error::Category::Transport, 75, "finite attempt one"});
            waitUntil(
                "finite recovery attempt one is waiting",
                [this]() {
                    return scheduler.entries.size() == 4 &&
                           backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Recovering;
                },
                [this]() {
                    respondToInitialize = false;
                    scheduler.fire(3);
                    waitUntil(
                        "finite attempt one starts a provider",
                        [this]() {
                            return backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Initializing;
                        },
                        [this]() {
                            transport->callbacks.onError(Error{Error::Category::Process, 76, "finite attempt two"});
                            waitUntil(
                                "finite recovery attempt two is waiting",
                                [this]() {
                                    return scheduler.entries.size() == 5 &&
                                           backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Recovering;
                                },
                                [this]() {
                                    scheduler.fire(4);
                                    waitUntil(
                                        "finite attempt two starts a provider",
                                        [this]() {
                                            return backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Initializing;
                                        },
                                        [this]() {
                                            transport->callbacks.onError(
                                                Error{Error::Category::Transport, 77, "finite attempts exhausted"});
                                            waitUntil(
                                                "finite recovery policy becomes explicitly exhausted",
                                                [this]() {
                                                    const backend::Snapshot snapshot = backendCore->snapshot();
                                                    return snapshot.provider.lifecycle == backend::ProviderLifecycle::Failed &&
                                                           snapshot.provider.recovery.status == backend::RecoveryStatus::Exhausted;
                                                },
                                                [this]() {
                                                    respondToInitialize = true;
                                                    backendCore->restart();
                                                    waitUntil(
                                                        "manual restart resets an exhausted recovery policy",
                                                        [this]() {
                                                            return backendCore->isReady();
                                                        },
                                                        [this]() {
                                                            const backend::Snapshot ready = backendCore->snapshot();
                                                            expect(ready.provider.recovery.status == backend::RecoveryStatus::Idle &&
                                                                       ready.provider.recovery.attempts == 0,
                                                                   "manual restart clears Exhausted and successful Ready resets attempts");
                                                            verifyIneligibleFailureDuringRecoveryAttempt();
                                                        });
                                                });
                                        });
                                });
                        });
                });
        }

        void verifyIneligibleFailureDuringRecoveryAttempt() {
            const std::size_t timerIndex = scheduler.entries.size();
            transport->callbacks.onError(Error{Error::Category::Transport, 78, "retry setup failure"});
            waitUntil(
                "eligible failure schedules a retry for in-attempt classification",
                [this, timerIndex]() {
                    return scheduler.entries.size() == timerIndex + 1 &&
                           backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Recovering;
                },
                [this, timerIndex]() {
                    respondToInitialize = false;
                    scheduler.fire(timerIndex);
                    waitUntil(
                        "the scheduled retry reaches Initializing",
                        [this]() {
                            return backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Initializing;
                        },
                        [this, timerIndex]() {
                            transport->callbacks.onError(
                                Error{Error::Category::Protocol, 81, "ineligible failure during recovery attempt"});
                            waitUntil(
                                "ineligible recovery-attempt failure settles without a pending timer",
                                [this]() {
                                    const backend::Snapshot snapshot = backendCore->snapshot();
                                    return snapshot.provider.lifecycle == backend::ProviderLifecycle::Failed &&
                                           snapshot.provider.recovery.status == backend::RecoveryStatus::Idle &&
                                           !snapshot.provider.recovery.delayMs;
                                },
                                [this, timerIndex]() {
                                    expect(scheduler.entries.size() == timerIndex + 1,
                                           "an ineligible failure during a retry attempt does not schedule another retry");
                                    respondToInitialize = true;
                                    backendCore->restart();
                                    waitUntil(
                                        "manual restart recovers the ineligible retry-attempt failure",
                                        [this]() {
                                            return backendCore->isReady();
                                        },
                                        [this]() {
                                            verifyStopCancelsRecovery();
                                        });
                                });
                        });
                });
        }

        void verifyStopCancelsRecovery() {
            const std::size_t timerIndex = scheduler.entries.size();
            transport->callbacks.onError(Error{Error::Category::Transport, 74, "stop cancellation failure"});
            waitUntil(
                "stop cancellation has one pending recovery timer",
                [this, timerIndex]() {
                    return scheduler.entries.size() == timerIndex + 1 &&
                           backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Recovering;
                },
                [this, timerIndex]() {
                    const std::uint64_t generation = backendCore->snapshot().provider.generation;
                    const std::size_t starts = transport->startCount;
                    backendCore->stop();
                    scheduler.fire(timerIndex, true);
                    defer([this, generation, starts]() {
                        const backend::Snapshot stopped = backendCore->snapshot();
                        expect(stopped.provider.lifecycle == backend::ProviderLifecycle::Stopped && !stopped.provider.desiredRunning &&
                                   stopped.provider.recovery.status == backend::RecoveryStatus::Idle &&
                                   stopped.provider.generation == generation && transport->startCount == starts,
                               "manual stop cancels recovery without incrementing provider generation");
                        expect(session.isOpen() && session.role() == backend::SessionRole::Controller,
                               "stopping the provider does not close the backend service session or release its controller");
                        backendCore->restart();
                        waitUntil(
                            "provider restarts after manual stop for pending-request overflow test",
                            [this]() {
                                return backendCore->isReady();
                            },
                            [this]() {
                                verifyPendingRequestOverflow();
                            });
                    });
                });
        }

        void verifyPendingRequestOverflow() {
            transport->inject({{"method", "future/request-one"}, {"id", "one"}, {"params", Json::object()}});
            waitUntil(
                "an unknown future request occupies the global pending capacity",
                [this]() {
                    return backendCore->snapshot().pendingRequests.size() == 1;
                },
                [this]() {
                    backendCore->restart();
                    waitUntil(
                        "provider invalidation retires the retained unknown occurrence before restart",
                        [this]() {
                            const backend::Snapshot snapshot = backendCore->snapshot();
                            return backendCore->isReady() && snapshot.pendingRequests.empty();
                        },
                        [this]() {
                            verifyDeferredPendingRequestOverflow();
                        });
                });
        }

        void verifyDeferredPendingRequestOverflow() {
            const std::size_t recoveryTimers = scheduler.entries.size();
            transport->inject({{"method", "attestation/generate"}, {"id", "attestation-one"}, {"params", Json::object()}});
            waitUntil(
                "an A1.6b-deferred attestation request occupies one pending slot",
                [this]() {
                    return backendCore->snapshot().pendingRequests.size() == 1;
                },
                [this, recoveryTimers]() {
                    const backend::Snapshot retained = backendCore->snapshot();
                    expect(retained.pendingRequests.front().type == "attestation" && retained.pendingRequests.front().details.empty(),
                           "an attestation occurrence has a meaningful bounded projection without provider request identifiers");
                    transport->inject({{"method", "item/tool/call"},
                                       {"id", "dynamic-two"},
                                       {"params",
                                        {{"arguments", {{"safe", true}}},
                                         {"callId", "call"},
                                         {"threadId", "thread"},
                                         {"tool", "tool"},
                                         {"turnId", "turn"}}}});
                    waitUntil(
                        "a second deferred typed request fails closed at provider scope",
                        [this]() {
                            const backend::Snapshot snapshot = backendCore->snapshot();
                            return snapshot.provider.lifecycle == backend::ProviderLifecycle::Failed && snapshot.pendingRequests.empty();
                        },
                        [this, recoveryTimers]() {
                            afterTicks(4, [this, recoveryTimers]() {
                                const backend::Snapshot failed = backendCore->snapshot();
                                const bool automaticResponse =
                                    std::any_of(transport->outgoing.begin(), transport->outgoing.end(), [](const Json& message) {
                                        return message.contains("id") &&
                                               (message.at("id") == "attestation-one" || message.at("id") == "dynamic-two") &&
                                               (message.contains("result") || message.contains("error"));
                                    });
                                expect(failed.provider.lifecycle == backend::ProviderLifecycle::Failed && !transport->running &&
                                           failed.capacity.state.providerRequestOverflows == 1 && failed.provider.lastError &&
                                           failed.provider.lastError->category == "capacity" &&
                                           scheduler.entries.size() == recoveryTimers && !automaticResponse,
                                       "deferred request overflow retains no occurrence, emits no automatic answer, and schedules no "
                                       "recovery");
                                expect(session.isOpen() && session.role() == backend::SessionRole::Controller,
                                       "pending-request overflow retains service sessions and controller ownership");
                                beginUnlimitedRecoveryScenario();
                            });
                        });
                });
        }

        void beginUnlimitedRecoveryScenario() {
            observer.close();
            session.close("finite recovery scenario complete");
            backendCore.reset();
            scheduler.entries.clear();
            respondToInitialize = true;
            configureTransport();

            backend::BackendCoreOptions options;
            options.initialThreadListLimit = 0;
            options.recovery = {true, 0, 10, 0, 0};
            options.recoveryTimerScheduler = [this](std::uint64_t delayMs, std::function<void()> callback) {
                return scheduler.schedule(delayMs, std::move(callback));
            };
            backendCore = std::make_unique<FakeBackendCore>(std::move(options), transport);
            backendCore->start();
            waitUntil(
                "unlimited-recovery provider reaches its initial Ready state",
                [this]() {
                    return backendCore->isReady();
                },
                [this]() {
                    verifyUnlimitedAttempt(1);
                });
        }

        void verifyUnlimitedAttempt(std::uint32_t attempt) {
            respondToInitialize = false;
            transport->callbacks.onError(Error{Error::Category::Transport, static_cast<int>(100 + attempt), "unlimited recovery failure"});
            waitUntil(
                "unlimited zero-delay recovery attempt is scheduled",
                [this, attempt]() {
                    return scheduler.entries.size() == attempt &&
                           backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Recovering;
                },
                [this, attempt]() {
                    const backend::Snapshot waiting = backendCore->snapshot();
                    expect(waiting.provider.recovery.attempts == attempt && waiting.provider.recovery.delayMs == 0 &&
                               scheduler.entries[attempt - 1].delayMs == 0,
                           "zero-delay unlimited recovery remains asynchronous and retains monotonic attempt accounting");
                    const std::uint64_t generation = waiting.provider.generation;
                    if (attempt == 3) {
                        respondToInitialize = true;
                    }
                    scheduler.fire(attempt - 1);
                    expect(backendCore->snapshot().provider.generation == generation,
                           "a normalized zero-delay recovery callback never starts the provider inline");
                    if (attempt == 3) {
                        waitUntil(
                            "unlimited recovery succeeds after more than a finite default attempt count",
                            [this, generation]() {
                                return backendCore->isReady() && backendCore->snapshot().provider.generation == generation + 1;
                            },
                            [this]() {
                                const backend::Snapshot ready = backendCore->snapshot();
                                expect(ready.provider.recovery.status == backend::RecoveryStatus::Idle &&
                                           ready.provider.recovery.attempts == 0,
                                       "maximumAttempts zero permits repeated retries and Ready resets their state");
                                beginZeroCapacityHydrationScenario();
                            });
                    } else {
                        waitUntil(
                            "unlimited recovery starts the next provider attempt",
                            [this, generation]() {
                                return backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Initializing &&
                                       backendCore->snapshot().provider.generation == generation + 1;
                            },
                            [this, attempt]() {
                                verifyUnlimitedAttempt(attempt + 1);
                            });
                    }
                });
        }

        void beginZeroCapacityHydrationScenario() {
            backendCore->stop();
            backendCore.reset();
            scheduler.entries.clear();
            respondToInitialize = true;
            holdHydration = true;
            pendingHydrationId.reset();
            pendingHydrationCallbacks = {};
            configureTransport();

            backend::BackendCoreOptions options;
            options.initialThreadListLimit = 1;
            options.capacity.maxActiveOperations = 0;
            options.recoveryTimerScheduler = [this](std::uint64_t delayMs, std::function<void()> callback) {
                return scheduler.schedule(delayMs, std::move(callback));
            };
            backendCore = std::make_unique<FakeBackendCore>(std::move(options), transport);
            backendCore->start();
            waitUntil(
                "zero-capacity provider reaches Ready without starting hydration",
                [this]() {
                    return backendCore->isReady() && backendCore->snapshot().diagnostics.received == 1;
                },
                [this]() {
                    const backend::Snapshot ready = backendCore->snapshot();
                    const std::size_t hydrationRequests =
                        std::count_if(transport->outgoing.begin(), transport->outgoing.end(), [](const Json& message) {
                            return message.value("method", "") == "thread/list";
                        });
                    expect(hydrationRequests == 0 && ready.capacity.state.rejectedOperations == 1 && ready.threadList.pagesLoaded == 0 &&
                               ready.threadList.stamp.freshness == backend::Freshness::Unknown &&
                               ready.provider.lifecycle == backend::ProviderLifecycle::Ready && scheduler.entries.empty() &&
                               !ready.diagnostics.recent.empty() &&
                               ready.diagnostics.recent.back().find("provider-operation capacity") != std::string::npos,
                           "zero global operation capacity skips hydration once, records rejection and diagnostic, and keeps Ready");
                    beginStaleHydrationScenario();
                });
        }

        void beginStaleHydrationScenario() {
            backendCore->stop();
            backendCore.reset();
            holdHydration = true;
            pendingHydrationId.reset();
            pendingHydrationCallbacks = {};
            staleHydrationId.reset();
            staleHydrationCallbacks = {};
            staleCapacityOperation.reset();
            configureTransport();

            backend::BackendCoreOptions options;
            options.initialThreadListLimit = 1;
            options.capacity.maxActiveOperations = 1;
            backendCore = std::make_unique<FakeBackendCore>(std::move(options), transport);
            backend::FrontendSessionCallbacks callbacks;
            callbacks.onCommandCompleted = [this](const backend::CommandCompletion& completion) {
                if (completion.requestId == "stale-capacity-operation") {
                    staleCapacityOperation = completion;
                }
            };
            session = backendCore->openSession(std::move(callbacks));
            session.submit("stale-controller", backend::ControllerAcquire{});
            backendCore->start();
            waitUntil(
                "generation one holds its internal hydration slot",
                [this]() {
                    return backendCore->isReady() && pendingHydrationId.has_value();
                },
                [this]() {
                    staleHydrationId = std::move(pendingHydrationId);
                    staleHydrationCallbacks = std::move(pendingHydrationCallbacks);
                    pendingHydrationId.reset();
                    pendingHydrationCallbacks = {};
                    const std::uint64_t generation = backendCore->snapshot().provider.generation;
                    backendCore->restart();
                    waitUntil(
                        "provider invalidation clears old hydration accounting and admits one new-generation hydration",
                        [this, generation]() {
                            return backendCore->isReady() && backendCore->snapshot().provider.generation == generation + 1 &&
                                   pendingHydrationId.has_value();
                        },
                        [this]() {
                            injectStaleHydrationCompletion();
                        });
                });
        }

        void injectStaleHydrationCompletion() {
            if (staleHydrationId) {
                tests::codex::inject(staleHydrationCallbacks,
                                     Json{{"id", *staleHydrationId},
                                          {"result", {{"data", Json::array()}, {"nextCursor", nullptr}, {"backwardsCursor", nullptr}}}});
            }
            backend::ThreadList command;
            session.submit("stale-capacity-operation", std::move(command));
            waitUntil(
                "stale hydration completion cannot release the current generation slot",
                [this]() {
                    return staleCapacityOperation.has_value();
                },
                [this]() {
                    const backend::Snapshot beforeCurrentCompletion = backendCore->snapshot();
                    expect(staleCapacityOperation->result.error &&
                               staleCapacityOperation->result.error->code == backend::CommandErrorCode::LocalSubmissionFailure &&
                               beforeCurrentCompletion.threadList.pagesLoaded == 0 && pendingHydrationId.has_value(),
                           "an old-generation hydration callback neither mutates freshness nor releases newer accounting");
                    releaseHydration();
                    waitUntil(
                        "current-generation hydration completes after stale callback isolation",
                        [this]() {
                            const backend::Snapshot snapshot = backendCore->snapshot();
                            return snapshot.threadList.pagesLoaded == 1 &&
                                   snapshot.threadList.stamp ==
                                       backend::SourceStamp{snapshot.provider.generation, backend::Freshness::Current};
                        },
                        [this]() {
                            finish();
                        });
                });
        }

        void finish() {
            if (isFinished) {
                return;
            }
            isFinished = true;
            if (backendCore) {
                backendCore->stop();
            }
            core::SNodeC::stop();
            testProviderStartRejectedDuringShutdown(result);
        }

        tests::support::TestResult& result;
        std::shared_ptr<tests::codex::FakeTransportState> transport;
        ManualRecoveryScheduler scheduler;
        std::unique_ptr<FakeBackendCore> backendCore;
        backend::FrontendSession session;
        backend::BackendObserverSubscription observer;
        std::optional<backend::CommandCompletion> capacityOperation;
        std::optional<backend::CommandCompletion> postHydrationOperation;
        std::optional<Json> pendingHydrationId;
        TransportCallbacks pendingHydrationCallbacks;
        std::optional<Json> staleHydrationId;
        TransportCallbacks staleHydrationCallbacks;
        std::optional<backend::CommandCompletion> staleCapacityOperation;
        bool holdHydration = true;
        bool respondToInitialize = true;
        bool duplicateRestartDuringReplacementInitialization = true;
        std::uint64_t eligibleBaseGeneration = 0;
        std::size_t eligibleBaseStartCount = 0;
        bool isFinished = false;
    };
} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    int returnCode = tests::support::cTestSkipReturnCode;
    if (tests::support::shouldSkipRootWithoutSNodeCGroup()) {
        tests::support::printRootWithoutSNodeCGroupSkipMessage("CodexBackendFoundationTest");
    } else {
        core::SNodeC::init(argc, argv);
        testRecoveryPolicyEligibility(result);
        testTargetedItemSnapshotBatch(result);
        testReducerCapacityAndFreshness(result);
        testIncrementalRetentionAndFreshness(result);
        testZeroHandleCapacities(result);
        bool timedOut = false;
        FoundationRunner runner(result);
        [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
            [&timedOut]() {
                timedOut = true;
                core::SNodeC::stop();
            },
            utils::Timeval({8, 0}));
        runner.start();
        const int eventLoopResult = core::SNodeC::start(utils::Timeval({10, 0}));
        result.expectTrue(!timedOut, "Backend foundation recovery scenario finishes before watchdog");
        result.expectTrue(runner.finished(), "Backend foundation recovery scenario reaches deterministic completion");
        result.expectEqual(0, eventLoopResult, "Backend foundation recovery event loop exits cleanly");
        core::SNodeC::free();
        returnCode = result.processResult();
    }
    return returnCode;
}
