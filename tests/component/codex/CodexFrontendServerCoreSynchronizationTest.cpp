/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/internal/server/BackendProjection.h"
#include "ai/openai/codex/frontend/internal/server/ServerCore.h"
#include "support/TestResult.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ai::openai::codex::frontend::internal::server {
    struct BackendProjectionTestAccess {
        static model::ModelResult<ProjectedBackendBatch>
        projectActivity(const BackendProjection& projection,
                        const backend::Snapshot& snapshot,
                        std::optional<std::string_view> key) {
            return projection.projectActivityForTesting(snapshot, key);
        }
    };
} // namespace ai::openai::codex::frontend::internal::server

namespace {
    namespace backend = ai::openai::codex::backend;
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = ai::openai::codex::frontend::generated;
    namespace model = ai::openai::codex::frontend::internal::model;
    namespace server = ai::openai::codex::frontend::internal::server;
    namespace typed = ai::openai::codex::typed;

    constexpr std::string_view OpaqueDataKey = "futureProviderState";
    constexpr std::string_view OpaqueDataValue = "safe-looking-but-opaque-sentinel";
    constexpr std::string_view OpaqueExtensionKey = "futureProviderExtension";
    constexpr std::string_view OpaqueExtensionValue = "safe-looking-extension-sentinel";
    constexpr std::string_view SecretDataValue = "known-secret-item-sentinel";

    class Backend final : public server::BackendPort {
    public:
        [[nodiscard]] bool providerReady() const noexcept override {
            return true;
        }

        [[nodiscard]] model::CanonicalSnapshot snapshot() const override {
            return state;
        }

        [[nodiscard]] server::BackendSubmitStatus submit(server::BackendInvocation invocation) override {
            if (core && (invocation.token.method == generated::MethodId::ControllerAcquire ||
                         invocation.token.method == generated::MethodId::ControllerRelease)) {
                const bool acquire = invocation.token.method == generated::MethodId::ControllerAcquire;
                static_cast<void>(core->complete(server::BackendCompletion{
                    invocation.token,
                    server::BackendCommandSuccess{generated::makeResult(
                        invocation.token.method,
                        acquire ? frontend::Json{{"controllerSessionId", invocation.session.value()}, {"role", "controller"}}
                                : frontend::Json{{"role", "observer"}})}}));
            }
            return server::BackendSubmitStatus::Accepted;
        }

        void bind(server::ServerCore& boundCore) noexcept override {
            core = &boundCore;
        }

        void unbind(server::ServerCore& boundCore) noexcept override {
            if (core == &boundCore) {
                core = nullptr;
            }
        }

        [[nodiscard]] bool performProviderLifecycleAction(server::ProviderLifecycleAction) override {
            return true;
        }

        model::CanonicalSnapshot state;
        server::ServerCore* core = nullptr;
    };

    frontend::AuthenticationResult authenticate(const frontend::FrontendPeerContext&, const frontend::AuthenticationCredential&) {
        frontend::FrontendPrincipal principal;
        principal.id = "synchronization-principal";
        principal.profile = "test";
        principal.scopes = {frontend::FrontendScope::Observe};
        return frontend::AuthenticationSuccess{std::move(principal)};
    }

    server::ConnectionCallbacks collect(std::vector<frontend::ServerMessage>& messages) {
        return {[&messages](const frontend::ServerMessage& message) {
                    messages.push_back(message);
                    return true;
                },
                [](const server::ConnectionClose&) {
                }};
    }

    bool excludesOpaqueStrings(const std::vector<frontend::ServerMessage>& messages, std::string_view key, std::string_view value) {
        for (const frontend::ServerMessage& message : messages) {
            const auto encoded = frontend::Codec::encodeServer(message);
            if (!encoded) {
                return false;
            }
            const std::string serialized = encoded.value().dump();
            if (serialized.find(key) != std::string::npos || serialized.find(value) != std::string::npos) {
                return false;
            }
        }
        return true;
    }

    model::OccurrenceDraft providerOccurrence(std::string source, bool ready) {
        model::ProviderState provider;
        provider.lifecycle = ready ? model::ProviderLifecycle::Ready : model::ProviderLifecycle::Stopped;
        return {model::SourceStamp{"backend-event:" + source}, model::ProviderUpdatedOccurrence{std::move(provider)}};
    }

    model::OccurrenceDraft processOccurrence(std::string source, std::string handle) {
        model::ProcessState process{model::ProcessHandle{std::move(handle)}};
        process.lifecycle = "running";
        return {model::SourceStamp{"backend-event:" + source}, model::ProcessUpdatedOccurrence{std::move(process)}};
    }

    model::OccurrenceDraft expandedGroup() {
        model::ConfigurationState configuration;
        configuration.state = model::DomainState::present();
        configuration.state.summary = "configuration warning";
        model::NoticeRecord notice;
        notice.occurrence = 1;
        notice.category = "configuration";
        notice.summary = "configuration warning";
        model::LegacyCompatibilityPayload legacy;
        legacy.kind = model::LegacyCompatibilityKind::CodexExtension;
        legacy.sourcePayloadIndex = 0;
        model::LegacySafeExtension extension;
        extension.method = "configWarning";
        extension.params = *model::SafeDetail::fromJson(frontend::Json{{"message", "configuration warning"}});
        legacy.safeExtension = std::move(extension);
        std::vector<model::OccurrencePayload> expanded;
        expanded.emplace_back(model::ConfigurationUpdatedOccurrence{std::move(configuration)});
        expanded.emplace_back(model::NoticeAddedOccurrence{std::move(notice)});
        return {model::SourceStamp{"server_notification:ServerNotification:method:configWarning"}, std::move(legacy), std::move(expanded)};
    }

    model::OccurrenceDraft containedUnknownOccurrence() {
        model::LegacyCompatibilityPayload legacy;
        legacy.kind = model::LegacyCompatibilityKind::CodexExtension;
        legacy.sourcePayloadIndex = 0;
        model::LegacySafeExtension extension;
        extension.method = "futureNotification";
        extension.params = *model::SafeDetail::fromJson(frontend::Json{{"safe", "detail"}});
        legacy.safeExtension = std::move(extension);
        return {model::SourceStamp{"server_notification:unknown:futureNotification"}, std::move(legacy), {}};
    }

    model::OccurrenceDraft legacyItemOccurrence() {
        model::LegacyItemCompatibility item{
            model::ItemData{model::ItemIdentity{"future-live-item"},
                            model::ThreadIdentity{"future-live-thread"},
                            model::TurnIdentity{"future-live-turn"}},
            "future_codex_item_kind",
            0,
            "/threads/0/turns/0/items/0"};
        item.value.sourceIndex = 0;
        model::LegacyCompatibilityPayload legacy;
        legacy.kind = model::LegacyCompatibilityKind::LegacyItem;
        legacy.legacyItem = std::move(item);
        return {model::SourceStamp{"backend-event:future-live-item"}, std::move(legacy), {}};
    }

    model::OccurrenceDraft legacyPendingOccurrence() {
        model::LegacyPendingRequestCompatibility request{
            model::PendingRequestData{model::PendingRequestIdentity{"172"}}, 0, "/pendingRequests/0"};
        request.value.sourceIndex = 0;
        request.value.safeDetails =
            *model::SafeDetail::fromJson(frontend::Json{{"method", "future/serverRequest"}, {"sensitiveFieldsRedacted", true}});
        model::LegacyCompatibilityPayload legacy;
        legacy.kind = model::LegacyCompatibilityKind::LegacyPendingRequest;
        legacy.legacyPendingRequest = std::move(request);
        return {model::SourceStamp{"backend-event:future-pending"}, std::move(legacy), {}};
    }

    model::OccurrenceDraft itemContentOccurrence() {
        model::ItemContentUpdatedOccurrence update{model::ItemIdentity{"item-mixed"}};
        update.threadId = model::ThreadIdentity{"thread-mixed"};
        update.turnId = model::TurnIdentity{"turn-mixed"};
        update.channel = "agentText";
        update.content = "mixed representation";
        model::LegacyCompatibilityPayload legacy;
        legacy.kind = model::LegacyCompatibilityKind::ItemContentUpdated;
        legacy.sourcePayloadIndex = 0;
        std::vector<model::OccurrencePayload> expanded;
        expanded.emplace_back(std::move(update));
        return {model::SourceStamp{"server_notification:ServerNotification:method:item/agentMessage/delta"},
                std::move(legacy),
                std::move(expanded)};
    }

    model::OccurrenceDraft itemOccurrence(std::string source, std::string status, std::string text) {
        model::ItemData data{model::ItemIdentity{"item-order"},
                             model::ThreadIdentity{"thread-order"},
                             model::TurnIdentity{"turn-order"}};
        data.status = std::move(status);
        data.agentText = std::move(text);
        model::OccurrenceDraft occurrence{model::SourceStamp{"backend-event:" + source},
                                          model::ItemUpsertedOccurrence{model::AgentMessageItem{std::move(data)}}};
        occurrence.threadId = model::ThreadIdentity{"thread-order"};
        occurrence.turnId = model::TurnIdentity{"turn-order"};
        occurrence.itemId = model::ItemIdentity{"item-order"};
        return occurrence;
    }

    model::OccurrenceDraft orderedItemContentOccurrence() {
        model::ItemContentUpdatedOccurrence update{model::ItemIdentity{"item-order"}};
        update.threadId = model::ThreadIdentity{"thread-order"};
        update.turnId = model::TurnIdentity{"turn-order"};
        update.channel = "agentText";
        update.content = "final text";
        model::OccurrenceDraft occurrence{model::SourceStamp{"backend-event:1002"}, std::move(update)};
        occurrence.threadId = model::ThreadIdentity{"thread-order"};
        occurrence.turnId = model::TurnIdentity{"turn-order"};
        occurrence.itemId = model::ItemIdentity{"item-order"};
        return occurrence;
    }

    model::OccurrenceDraft configurationOccurrence() {
        model::ConfigurationState configuration;
        configuration.state = model::DomainState::present();
        return {model::SourceStamp{"backend-event:900"},
                model::ConfigurationUpdatedOccurrence{std::move(configuration)}};
    }

    model::OccurrenceDraft modelsOccurrence() {
        model::ModelsState models;
        models.state = model::DomainState::present();
        return {model::SourceStamp{"backend-event:901"}, model::ModelsUpdatedOccurrence{std::move(models)}};
    }

    void drainOne(std::vector<std::function<void()>>& scheduled) {
        std::function<void()> callback = std::move(scheduled.front());
        scheduled.erase(scheduled.begin());
        callback();
    }

    void drainAll(std::vector<std::function<void()>>& scheduled) {
        std::size_t turns = 0;
        while (!scheduled.empty()) {
            drainOne(scheduled);
            if (++turns > 64) {
                throw std::runtime_error("synchronization scheduler did not quiesce");
            }
        }
    }

    void testSynchronization(tests::support::TestResult& result) {
        Backend backend;
        server::ServerCoreOptions options;
        options.authenticator = authenticate;
        options.journalInitialSequence = model::FrontendSequence{5};
        options.maxInboundBurst = 1000;
        server::ServerCore core(backend, std::move(options));
        core.start();

        const server::PublishResult retained = core.publishGroup(providerOccurrence("6", false));
        result.expectTrue(retained.accepted && !retained.error.has_value() &&
                              retained.deliveryMode == server::PublishDeliveryMode::Occurrences &&
                              retained.sequence == model::FrontendSequence{6} && core.currentSequence() == retained.sequence,
                          "the typed journal advances exactly once from the configured initial sequence");

        std::vector<frontend::ServerMessage> expandedMessages;
        const auto expandedConnection = core.openConnection({}, collect(expandedMessages));
        frontend::Hello replayHello;
        replayHello.resumeAfter = frontend::SequenceNumber{5};
        replayHello.capabilities = std::vector{frontend::FrontendCapability::CompleteBackendDomains,
                                               frontend::FrontendCapability::DedicatedNotificationEvents};
        const auto replayAccepted = core.receive(*expandedConnection, frontend::ClientMessage{std::move(replayHello)});
        const auto* replayWelcome = !expandedMessages.empty() ? std::get_if<frontend::Welcome>(&expandedMessages.front()) : nullptr;
        const auto* replayBatch = expandedMessages.size() > 1 ? std::get_if<frontend::EventBatch>(&expandedMessages[1]) : nullptr;
        result.expectTrue(replayAccepted.accepted() && replayWelcome && replayWelcome->syncMode == frontend::SyncMode::Replay &&
                              replayBatch && replayBatch->fromSequence == frontend::SequenceNumber{6} &&
                              replayBatch->toSequence == frontend::SequenceNumber{6} && replayBatch->events.size() == 1 &&
                              expandedMessages.size() == 4 &&
                              std::holds_alternative<frontend::SyncComplete>(expandedMessages[2]) &&
                              std::holds_alternative<frontend::EventBatch>(expandedMessages[3]),
                          "initial replay completes before the newly joined session occurrence is delivered live");

        std::vector<frontend::ServerMessage> legacyMessages;
        const auto legacyConnection = core.openConnection({}, collect(legacyMessages));
        const bool legacyAccepted = core.receive(*legacyConnection, frontend::ClientMessage{frontend::Hello{}}).accepted();
        const frontend::Snapshot* legacySnapshot =
            legacyMessages.size() > 1 ? std::get_if<frontend::Snapshot>(&legacyMessages[1]) : nullptr;
        result.expectTrue(legacyAccepted && legacyMessages.size() == 4 && legacySnapshot &&
                              legacySnapshot->state.contains("backendRevision") &&
                              legacySnapshot->state.contains("lifecycle") && legacySnapshot->state.contains("diagnostics") &&
                              legacySnapshot->state.contains("journal") && !legacySnapshot->state.contains("provider") &&
                              !legacySnapshot->state.contains("controller") && !legacySnapshot->state.contains("truncation"),
                          "a client without representation capabilities receives the frozen legacy snapshot shape, not expanded state");

        expandedMessages.clear();
        legacyMessages.clear();
        const server::PublishResult live = core.publishGroup(expandedGroup());
        const auto* expandedBatch = expandedMessages.size() == 1 ? std::get_if<frontend::EventBatch>(&expandedMessages.front()) : nullptr;
        const auto* legacyBatch = legacyMessages.size() == 1 ? std::get_if<frontend::EventBatch>(&legacyMessages.front()) : nullptr;
        result.expectTrue(
            live.accepted && live.sequence == model::FrontendSequence{9} && expandedBatch && expandedBatch->events.size() == 2 &&
                legacyBatch && legacyBatch->events.size() == 1 && expandedBatch->fromSequence == legacyBatch->fromSequence,
            "one canonical occurrence group is encoded atomically as all expanded events or one legacy event, never a mixture");

        expandedMessages.clear();
        legacyMessages.clear();
        backend.state.provider.lifecycle = model::ProviderLifecycle::Ready;
        const server::SnapshotPublishResult resynchronized = core.publishSnapshot(backend.state);
        result.expectTrue(resynchronized.accepted &&
                              resynchronized.sequence == model::FrontendSequence{live.sequence.value() + 1} &&
                              resynchronized.recipientCount == 2 && expandedMessages.size() == 1 && legacyMessages.size() == 1 &&
                              std::holds_alternative<frontend::Snapshot>(expandedMessages.front()) &&
                              std::holds_alternative<frontend::Snapshot>(legacyMessages.front()),
                          "authoritative resynchronization invalidates replay and broadcasts one live projected Snapshot per connection");

        std::vector<frontend::ServerMessage> gapMessages;
        const auto gapConnection = core.openConnection({}, collect(gapMessages));
        frontend::Hello gapHello;
        gapHello.resumeAfter = frontend::SequenceNumber{7};
        gapHello.capabilities = std::vector{frontend::FrontendCapability::CompleteBackendDomains};
        result.expectTrue(core.receive(*gapConnection, frontend::ClientMessage{std::move(gapHello)}).accepted(),
                          "a replay cursor before the resynchronization barrier remains a valid Hello");
        const auto* gapWelcome = !gapMessages.empty() ? std::get_if<frontend::Welcome>(&gapMessages.front()) : nullptr;
        result.expectTrue(gapWelcome && gapWelcome->syncMode == frontend::SyncMode::Snapshot && gapMessages.size() == 4 &&
                              std::holds_alternative<frontend::Snapshot>(gapMessages[1]),
                          "a replay gap deterministically falls back to Snapshot synchronization");

        expandedMessages.clear();
        legacyMessages.clear();
        const server::PublishResult unknown = core.publishGroup(containedUnknownOccurrence());
        const auto* expandedFallback =
            expandedMessages.size() == 1 ? std::get_if<frontend::EventBatch>(&expandedMessages.front()) : nullptr;
        const auto* legacyFallback = legacyMessages.size() == 1 ? std::get_if<frontend::EventBatch>(&legacyMessages.front()) : nullptr;
        result.expectTrue(unknown.accepted && unknown.occurrenceCount == 1 && expandedFallback && legacyFallback &&
                              expandedFallback->events.size() == 1 && legacyFallback->events.size() == 1 &&
                              expandedFallback->events.front().type == "codex.extension" &&
                              legacyFallback->events.front().type == "codex.extension",
                          "an unknown safe notification remains one bounded codex.extension even for expanded clients");
    }

    void testTypedBatching(tests::support::TestResult& result) {
        Backend backend;
        std::vector<std::function<void()>> scheduled;
        server::ServerCoreOptions options;
        options.authenticator = authenticate;
        options.maxEventsPerBatch = 2;
        options.scheduler = [&scheduled](std::function<void()> callback) {
            scheduled.push_back(std::move(callback));
        };
        server::ServerCore core(backend, std::move(options));
        core.start();

        std::vector<frontend::ServerMessage> liveMessages;
        const auto live = core.openConnection({}, collect(liveMessages));
        result.expectTrue(live && core.receive(*live, frontend::ClientMessage{frontend::Hello{}}).accepted() && !scheduled.empty(),
                          "manual scheduler captures the initial delivery turn");
        drainOne(scheduled);
        liveMessages.clear();

        result.expectTrue(core.publishGroup(providerOccurrence("101", false)).accepted &&
                              core.publishGroup(providerOccurrence("102", true)).accepted &&
                              core.publishGroup(providerOccurrence("103", false)).accepted && scheduled.size() == 1,
                          "three adjacent typed groups request one pending delivery turn");
        drainOne(scheduled);
        const auto* firstBatch = liveMessages.size() > 0 ? std::get_if<frontend::EventBatch>(&liveMessages[0]) : nullptr;
        const auto* secondBatch = liveMessages.size() > 1 ? std::get_if<frontend::EventBatch>(&liveMessages[1]) : nullptr;
        result.expectTrue(liveMessages.size() == 2 && firstBatch && secondBatch && firstBatch->events.size() == 2 &&
                              firstBatch->fromSequence == frontend::SequenceNumber{2} &&
                              firstBatch->toSequence == frontend::SequenceNumber{3} && secondBatch->events.size() == 1 &&
                              secondBatch->fromSequence == frontend::SequenceNumber{4},
                          "live delivery batches adjacent sequences by the configured event count without splitting a group");

        std::vector<frontend::ServerMessage> replayMessages;
        const auto replay = core.openConnection({}, collect(replayMessages));
        frontend::Hello replayHello;
        replayHello.resumeAfter = frontend::SequenceNumber{1};
        result.expectTrue(replay && core.receive(*replay, frontend::ClientMessage{std::move(replayHello)}).accepted() && !scheduled.empty(),
                          "retained groups are available to a later replay connection");
        drainOne(scheduled);
        const auto* replayFirst = replayMessages.size() > 1 ? std::get_if<frontend::EventBatch>(&replayMessages[1]) : nullptr;
        const auto* replaySecond = replayMessages.size() > 2 ? std::get_if<frontend::EventBatch>(&replayMessages[2]) : nullptr;
        result.expectTrue(replayMessages.size() == 5 && replayFirst && replaySecond && replayFirst->events.size() == 2 &&
                              replaySecond->events.size() == 1 &&
                              std::holds_alternative<frontend::SyncComplete>(replayMessages[3]) &&
                              std::holds_alternative<frontend::EventBatch>(replayMessages[4]),
                          "replay uses the same typed batch builder and exact live batch boundaries");

        Backend mixedBackend;
        std::vector<std::function<void()>> mixedScheduled;
        server::ServerCoreOptions mixedOptions;
        mixedOptions.authenticator = authenticate;
        mixedOptions.maxEventsPerBatch = 8;
        mixedOptions.scheduler = [&mixedScheduled](std::function<void()> callback) {
            mixedScheduled.push_back(std::move(callback));
        };
        server::ServerCore mixed(mixedBackend, std::move(mixedOptions));
        mixed.start();
        std::vector<frontend::ServerMessage> mixedMessages;
        const auto mixedConnection = mixed.openConnection({}, collect(mixedMessages));
        frontend::Hello mixedHello;
        mixedHello.capabilities = std::vector{frontend::FrontendCapability::DedicatedNotificationEvents};
        result.expectTrue(mixedConnection && mixed.receive(*mixedConnection, frontend::ClientMessage{std::move(mixedHello)}).accepted() &&
                              !mixedScheduled.empty(),
                          "mixed-capability client synchronizes with independent event-family capabilities");
        drainOne(mixedScheduled);

        std::vector<frontend::ServerMessage> legacyMixedMessages;
        const auto legacyMixedConnection = mixed.openConnection({}, collect(legacyMixedMessages));
        result.expectTrue(legacyMixedConnection &&
                              mixed.receive(*legacyMixedConnection, frontend::ClientMessage{frontend::Hello{}}).accepted() &&
                              !mixedScheduled.empty(),
                          "a legacy-capability connection synchronizes beside the expanded notification connection");
        drainOne(mixedScheduled);
        mixedMessages.clear();
        legacyMixedMessages.clear();
        result.expectTrue(mixed.publishGroup(providerOccurrence("201", true)).accepted &&
                              mixed.publishGroup(itemContentOccurrence()).accepted &&
                              mixed.publishGroup(configurationOccurrence()).accepted && mixed.publishGroup(modelsOccurrence()).accepted,
                          "mixed-capability occurrences enter one typed pending delivery turn");
        drainOne(mixedScheduled);
        const auto batchType = [](const frontend::ServerMessage& message) -> std::string {
            const auto* batch = std::get_if<frontend::EventBatch>(&message);
            return batch && !batch->events.empty() ? batch->events.front().type : std::string{};
        };
        result.expectTrue(mixedMessages.size() == 3 && batchType(mixedMessages[0]) == "provider.updated" &&
                              batchType(mixedMessages[1]) == "item.content.updated" &&
                              batchType(mixedMessages[2]) == "configuration.updated" &&
                              std::get<frontend::EventBatch>(mixedMessages[2]).events.size() == 2 &&
                              std::get<frontend::EventBatch>(mixedMessages[2]).events[1].type == "models.updated",
                          "independent capabilities force expanded/legacy/expanded representation switches into separate batches");
        const auto* legacyMixedBatch =
            legacyMixedMessages.size() == 1 ? std::get_if<frontend::EventBatch>(&legacyMixedMessages.front()) : nullptr;
        result.expectTrue(legacyMixedBatch && legacyMixedBatch->events.size() == 2 &&
                              legacyMixedBatch->events[0].type == "backend.lifecycle.changed" &&
                              legacyMixedBatch->events[1].type == "item.content.updated" && mixed.connectionOpen(*legacyMixedConnection),
                          "a connection without dedicated notification events omits direct-expanded domain occurrences without closing");

        Backend oversizedBackend;
        std::vector<std::function<void()>> oversizedScheduled;
        server::ServerCoreOptions oversizedOptions;
        oversizedOptions.authenticator = authenticate;
        oversizedOptions.maxEventsPerBatch = 1;
        oversizedOptions.scheduler = [&oversizedScheduled](std::function<void()> callback) {
            oversizedScheduled.push_back(std::move(callback));
        };
        server::ServerCore oversized(oversizedBackend, std::move(oversizedOptions));
        oversized.start();
        std::vector<frontend::ServerMessage> oversizedMessages;
        const auto oversizedConnection = oversized.openConnection({}, collect(oversizedMessages));
        frontend::Hello expandedHello;
        expandedHello.capabilities = std::vector{frontend::FrontendCapability::CompleteBackendDomains,
                                                 frontend::FrontendCapability::DedicatedNotificationEvents};
        result.expectTrue(oversizedConnection &&
                              oversized.receive(*oversizedConnection, frontend::ClientMessage{std::move(expandedHello)}).accepted() &&
                              !oversizedScheduled.empty(),
                          "expanded client synchronizes before the unsplittable-group probe");
        drainOne(oversizedScheduled);
        oversizedMessages.clear();

        backend::Snapshot postTurnBackend;
        backend::TurnSnapshot postTurn;
        postTurn.id = "post-turn";
        postTurn.threadId = "post-thread";
        postTurn.status = "completed";
        postTurn.terminal = true;
        postTurn.failure = frontend::Json{{"message", "safe failure"},
                                          {"additionalDetails", "bounded details"},
                                          {"codexErrorInfo", {{"httpConnectionFailed", {{"httpStatusCode", 503}}}}}};
        postTurn.tokenUsage = frontend::Json{
            {"last", {{"cachedInputTokens", 1}, {"inputTokens", 3}, {"outputTokens", 1}, {"reasoningOutputTokens", 2}, {"totalTokens", 4}}},
            {"modelContextWindow", 258'400},
            {"total",
             {{"cachedInputTokens", 2}, {"inputTokens", 7}, {"outputTokens", 2}, {"reasoningOutputTokens", 3}, {"totalTokens", 9}}}};
        backend::ThreadSnapshot postThread;
        postThread.id = "post-thread";
        postThread.realtime.lifecycle = "failed";
        postThread.realtime.lastError = "safe realtime failure";
        postThread.realtime.sessionId = "realtime-session";
        postThread.realtime.version = "v2";
        postThread.realtime.lastSdpBytes = 123;
        postThread.realtime.itemCount = 4;
        postThread.realtime.receivedAudioBytes = 5;
        postThread.realtime.droppedAudioBytes = 6;
        postThread.realtime.stamp = {7, backend::Freshness::Current};
        postThread.turns.push_back(std::move(postTurn));
        postTurnBackend.threads.push_back(std::move(postThread));
        postTurnBackend.provider.lastError = backend::ErrorSnapshot{"transport", 19, "bounded provider failure"};
        postTurnBackend.providerOperations.push_back({"thread/goal/set", 17, {8, backend::Freshness::Current}});
        postTurnBackend.conversations.latestGoalSet = backend::ConversationDomainSnapshot::GoalMutation{
            "set", "post-thread", "finish projection", "active", std::nullopt, {9, backend::Freshness::Current}};
        postTurnBackend.filesystem.latestResults.push_back(
            {"fs/readFile", 31, "completed", std::nullopt, "cursor", 1, true, {10, backend::Freshness::Stale}});
        postTurnBackend.capacity.state.rejectedSessions = 11;
        postTurnBackend.capacity.state.providerRequestOverflows = 12;
        postTurnBackend.capacity.state.evictedThreads = 13;
        postTurnBackend.capacity.state.droppedContentBytes = 14;
        postTurnBackend.capacity.state.snapshotOmissions = 15;
        postTurnBackend.capacity.state.evictedNotices = 16;
        postTurnBackend.capacity.state.droppedProcessOutputBytes = 17;
        postTurnBackend.capacity.state.evictedFuzzySearchSessions = 18;
        postTurnBackend.capacity.state.limits.maxSessions = 19;
        postTurnBackend.capacity.state.limits.maxSnapshotBytes = 20;
        postTurnBackend.capacity.state.limits.maxRetainedActivityRecords = 21;
        postTurnBackend.capacity.omittedThreads = 22;
        postTurnBackend.capacity.sourceSessionCount = 23;
        postTurnBackend.capacity.truncated = true;
        postTurnBackend.capacity.mandatoryCoreExceedsLimit = true;
        server::BackendProjection backendProjection;
        const model::ModelResult<model::CanonicalSnapshot> projectedPostTurn = backendProjection.projectSnapshot(postTurnBackend);
        const frontend::Json* semanticProjection = projectedPostTurn ? &projectedPostTurn.value().extensions.json() : nullptr;
        const frontend::Json* providerError = projectedPostTurn && projectedPostTurn.value().provider.lastError
                                                  ? &projectedPostTurn.value().provider.lastError->json()
                                                  : nullptr;
        const frontend::Json* realtime = projectedPostTurn ? &projectedPostTurn.value().threads.front().safeDetails.json().at("realtime")
                                                           : nullptr;
        const frontend::Json* failure = projectedPostTurn ? &projectedPostTurn.value().turns.front().safeDetails.json() : nullptr;
        const frontend::Json* capacity = semanticProjection && semanticProjection->contains("capacityProvenance")
                                             ? &semanticProjection->at("capacityProvenance")
                                             : nullptr;
        result.expectTrue(
            projectedPostTurn.hasValue() && semanticProjection && semanticProjection->contains("providerOperationsSemantic") &&
                semanticProjection->contains("conversationSemantic") && semanticProjection->contains("filesystemProviderSemantic") &&
                semanticProjection->contains("capacityProvenance") && providerError &&
                providerError->value("message", "") == "bounded provider failure" && realtime &&
                realtime->value("lastError", "") == "safe realtime failure" && failure &&
                failure->value("failureCodexErrorDiscriminator", "") == "httpConnectionFailed" &&
                failure->value("failureHttpStatusCode", 0) == 503 && capacity && capacity->value("rejectedSessions", 0) == 11 &&
                capacity->value("providerRequestOverflows", 0) == 12 && capacity->value("evictedThreads", 0) == 13 &&
                capacity->value("droppedContentBytes", 0) == 14 && capacity->value("snapshotOmissions", 0) == 15 &&
                capacity->value("evictedNotices", 0) == 16 && capacity->value("droppedProcessOutputBytes", 0) == 17 &&
                capacity->value("evictedFuzzySearchSessions", 0) == 18 && capacity->value("maxSessions", 0) == 19 &&
                capacity->value("maxSnapshotBytes", 0) == 20 && capacity->value("maxRetainedActivityRecords", 0) == 21 &&
                capacity->value("omittedThreads", 0) == 22 && capacity->value("sourceSessionCount", 0) == 23 &&
                capacity->value("truncated", false) && capacity->value("mandatoryCoreExceedsLimit", false),
            "bounded errors, domains, and complete retained capacity provenance project through v1 compatibility carriers");
        const auto projectedPostTurnWire =
            projectedPostTurn
                ? model::encodeProjectedSnapshot(projectedPostTurn.value(), model::SnapshotRepresentationSelection{true, true, true, true})
                : model::ModelResult<frontend::Snapshot>{projectedPostTurn.error()};
        result.expectTrue(projectedPostTurnWire.hasValue(),
                          projectedPostTurnWire ? "enriched Snapshot remains v1 encodable"
                                                : projectedPostTurnWire.error().path + ": " + projectedPostTurnWire.error().message);
        if (projectedPostTurn) {
            oversizedBackend.state = projectedPostTurn.value();
        }

        typed::Turn completedTurn;
        completedTurn.id = typed::TurnId{"post-turn"};
        completedTurn.threadId = typed::ThreadId{"post-thread"};
        completedTurn.status = typed::TurnStatus::completed();
        const std::vector<backend::SequencedBackendEvent> completedEvents{
            {backend::SequenceNumber{1}, backend::TurnCompleted{std::move(completedTurn)}}};
        model::ModelResult<server::ProjectedBackendBatch> projectedCompletion =
            backendProjection.projectOccurrences(completedEvents, postTurnBackend);
        std::vector<server::OccurrenceStageRequest> completionGroups;
        const bool projectedCompletionValid = projectedCompletion.hasValue();
        bool completionSnapshotRequired = false;
        if (projectedCompletion) {
            server::ProjectedBackendBatch completion = std::move(projectedCompletion).value();
            completionSnapshotRequired = completion.snapshotRequired;
            for (server::ProjectedBackendOccurrence& occurrence : completion.occurrences) {
                completionGroups.push_back(
                    {std::move(occurrence.key), std::move(occurrence.occurrence), occurrence.urgency});
            }
        }
        result.expectTrue(projectedCompletionValid && !completionSnapshotRequired && completionGroups.size() == 1 &&
                              oversized.stageGroups(std::move(completionGroups)).accepted() && !oversizedScheduled.empty(),
                          "the terminal turn occurrence remains representable after token usage is retained");
        drainOne(oversizedScheduled);
        result.expectTrue(oversizedMessages.size() == 1 && std::holds_alternative<frontend::EventBatch>(oversizedMessages.front()) &&
                              oversized.connectionOpen(*oversizedConnection),
                          "the terminal turn occurrence is journaled and delivered without an erroneous fallback");
        oversizedMessages.clear();

        const server::PublishResult oversizedPublished = oversized.publishGroup(expandedGroup());
        drainOne(oversizedScheduled);
        const auto* postTurnFallback =
            oversizedMessages.size() == 1 ? std::get_if<frontend::Snapshot>(&oversizedMessages.front()) : nullptr;
        const frontend::Json* postTurnUsage = nullptr;
        const frontend::Json* postTurnUsageLast = nullptr;
        const frontend::Json* postTurnUsageTotal = nullptr;
        if (postTurnFallback && postTurnFallback->state.contains("turns") && postTurnFallback->state.at("turns").size() == 1) {
            const frontend::Json& encodedTurn = postTurnFallback->state.at("turns").front();
            const auto usage = encodedTurn.find("tokenUsage");
            postTurnUsage = usage != encodedTurn.end() ? &*usage : nullptr;
            const auto last = encodedTurn.find("tokenUsageLast");
            postTurnUsageLast = last != encodedTurn.end() ? &*last : nullptr;
            const auto total = encodedTurn.find("tokenUsageTotal");
            postTurnUsageTotal = total != encodedTurn.end() ? &*total : nullptr;
        }
        result.expectTrue(oversizedPublished.accepted && oversizedMessages.size() == 1 && postTurnFallback && postTurnUsage &&
                              postTurnUsage->is_object() && !postTurnUsage->contains("last") && !postTurnUsage->contains("total") &&
                              postTurnUsageLast && postTurnUsageTotal && postTurnUsage->value("modelContextWindow", 0) == 258'400 &&
                              postTurnUsageLast->value("cachedInputTokens", 0) == 1 &&
                              postTurnUsageLast->value("reasoningOutputTokens", 0) == 2 &&
                              postTurnUsageTotal->value("inputTokens", 0) == 7 && postTurnUsageTotal->value("totalTokens", 0) == 9 &&
                              oversized.connectionOpen(*oversizedConnection),
                          "a post-turn Snapshot fallback keeps compatibility token usage and adds bounded typed sidecar positions");

        oversizedMessages.clear();
        generated::DefinedCommand explicitSnapshot{
            "post-turn-snapshot", generated::makeParameters(generated::MethodId::SnapshotGet, frontend::Json::object())};
        result.expectTrue(oversized.receiveDefinedCommand(*oversizedConnection, std::move(explicitSnapshot)).accepted() &&
                              !oversizedScheduled.empty(),
                          "the same connection accepts an explicit Snapshot after post-turn fallback");
        drainOne(oversizedScheduled);
        result.expectTrue(std::any_of(oversizedMessages.begin(),
                                     oversizedMessages.end(),
                                     [](const frontend::ServerMessage& message) {
                                         return std::holds_alternative<frontend::Snapshot>(message);
                                     }) &&
                              oversized.connectionOpen(*oversizedConnection),
                          "the explicit post-turn Snapshot encodes and leaves the original connection open");

        std::vector<frontend::ServerMessage> oversizedReplayMessages;
        const auto oversizedReplay = oversized.openConnection({}, collect(oversizedReplayMessages));
        frontend::Hello oversizedReplayHello;
        oversizedReplayHello.resumeAfter = frontend::SequenceNumber{0};
        oversizedReplayHello.capabilities = std::vector{frontend::FrontendCapability::CompleteBackendDomains,
                                                        frontend::FrontendCapability::DedicatedNotificationEvents};
        result.expectTrue(oversizedReplay &&
                              oversized.receive(*oversizedReplay, frontend::ClientMessage{std::move(oversizedReplayHello)}).accepted() &&
                              !oversizedScheduled.empty(),
                          "oversized retained occurrence is preflighted before the replay Welcome");
        drainOne(oversizedScheduled);
        const auto* oversizedWelcome =
            !oversizedReplayMessages.empty() ? std::get_if<frontend::Welcome>(&oversizedReplayMessages.front()) : nullptr;
        result.expectTrue(oversizedWelcome && oversizedWelcome->syncMode == frontend::SyncMode::Snapshot &&
                              oversizedReplayMessages.size() == 4 &&
                              std::holds_alternative<frontend::Snapshot>(oversizedReplayMessages[1]) &&
                              std::holds_alternative<frontend::SyncComplete>(oversizedReplayMessages[2]) &&
                              std::holds_alternative<frontend::EventBatch>(oversizedReplayMessages[3]),
                          "initial synchronization advertises Snapshot when projected replay cannot preserve an atomic group");
    }

    void testScopeFilteredSparseReplay(tests::support::TestResult& result) {
        Backend backend;
        backend.state.provider.lifecycle = model::ProviderLifecycle::Ready;
        backend.state.processes.emplace_back(model::ProcessHandle{"hidden-snapshot-process"});
        backend.state.processes.back().lifecycle = "running";
        server::ServerCoreOptions options;
        options.authenticator = authenticate;
        options.scheduler = [](std::function<void()> callback) {
            callback();
        };
        server::ServerCore core(backend, std::move(options));
        core.start();

        const server::PublishResult visibleFirst = core.publishGroup(providerOccurrence("301", false));
        const server::PublishResult hiddenMiddle = core.publishGroup(processOccurrence("302", "hidden-middle-process"));
        const server::PublishResult visibleSecond = core.publishGroup(providerOccurrence("303", true));
        const server::PublishResult hiddenSuffix = core.publishGroup(processOccurrence("304", "hidden-suffix-process"));
        result.expectTrue(visibleFirst.accepted && hiddenMiddle.accepted && visibleSecond.accepted && hiddenSuffix.accepted &&
                              visibleFirst.sequence == model::FrontendSequence{1} && hiddenMiddle.sequence == model::FrontendSequence{2} &&
                              visibleSecond.sequence == model::FrontendSequence{3} && hiddenSuffix.sequence == model::FrontendSequence{4},
                          "hidden scoped occurrences retain their canonical global sequence positions");

        std::vector<frontend::ServerMessage> replayMessages;
        const auto replay = core.openConnection({}, collect(replayMessages));
        frontend::Hello replayHello;
        replayHello.resumeAfter = frontend::SequenceNumber{0};
        replayHello.capabilities = {frontend::FrontendCapability::CompleteBackendDomains,
                                    frontend::FrontendCapability::DedicatedNotificationEvents};
        const bool replayAccepted = replay && core.receive(*replay, frontend::ClientMessage{std::move(replayHello)}).accepted();
        const auto* welcome = !replayMessages.empty() ? std::get_if<frontend::Welcome>(&replayMessages.front()) : nullptr;
        const auto* replayBatch = replayMessages.size() > 1 ? std::get_if<frontend::EventBatch>(&replayMessages[1]) : nullptr;
        const auto* synchronized = replayMessages.size() > 2 ? std::get_if<frontend::SyncComplete>(&replayMessages[2]) : nullptr;
        result.expectTrue(
            replayAccepted && welcome && welcome->syncMode == frontend::SyncMode::Replay &&
                welcome->currentSequence == frontend::SequenceNumber{4} && replayBatch &&
                replayBatch->fromSequence == frontend::SequenceNumber{1} && replayBatch->toSequence == frontend::SequenceNumber{3} &&
                replayBatch->events.size() == 2 && replayBatch->events[0].sequence == frontend::SequenceNumber{1} &&
                replayBatch->events[1].sequence == frontend::SequenceNumber{3} && synchronized &&
                synchronized->sequence == frontend::SequenceNumber{4} && replayMessages.size() == 4,
            "scope-filtered replay is sparse, while its cursor completes through the hidden suffix without a fabricated event");

        std::vector<frontend::ServerMessage> snapshotMessages;
        const auto snapshotConnection = core.openConnection({}, collect(snapshotMessages));
        frontend::Hello snapshotHello;
        snapshotHello.capabilities = {frontend::FrontendCapability::CompleteBackendDomains,
                                      frontend::FrontendCapability::DedicatedNotificationEvents};
        const bool snapshotAccepted =
            snapshotConnection && core.receive(*snapshotConnection, frontend::ClientMessage{std::move(snapshotHello)}).accepted();
        const auto* snapshot = snapshotMessages.size() > 1 ? std::get_if<frontend::Snapshot>(&snapshotMessages[1]) : nullptr;
        result.expectTrue(snapshotAccepted && snapshot && snapshot->sequence == frontend::SequenceNumber{5} &&
                              snapshot->state.contains("provider") && snapshot->state.at("provider").value("lifecycle", "") == "ready" &&
                              !snapshot->state.contains("processes"),
                          "Snapshot synchronization exposes the same authorized state while omitting the scoped process domain");
    }

    void testInlineObserverBatchCoalescing(tests::support::TestResult& result) {
        Backend backend;
        server::ServerCoreOptions options;
        options.authenticator = authenticate;
        options.scheduler = [](std::function<void()> callback) {
            callback();
        };
        server::ServerCore core(backend, std::move(options));
        core.start();

        std::vector<frontend::ServerMessage> messages;
        const auto connection = core.openConnection({}, collect(messages));
        const bool synchronized = connection && core.receive(*connection, frontend::ClientMessage{frontend::Hello{}}).accepted();
        messages.clear();
        const model::FrontendSequence before = core.currentSequence();

        server::OccurrenceCoalescingKey firstKey;
        firstKey.kind = server::OccurrenceEntityKind::BackendLifecycle;
        firstKey.entityId = "provider";
        server::OccurrenceCoalescingKey secondKey = firstKey;
        std::vector<server::OccurrenceStageRequest> batch;
        batch.push_back({std::move(firstKey), providerOccurrence("1001", false), server::OccurrenceFlushUrgency::Deferred});
        batch.push_back({std::move(secondKey), providerOccurrence("1002", true), server::OccurrenceFlushUrgency::Deferred});
        const server::OccurrenceStageResult staged = core.stageGroups(std::move(batch));

        const auto* delivered = messages.size() == 1 ? std::get_if<frontend::EventBatch>(&messages.front()) : nullptr;
        const std::uint64_t expectedSequence = before.value() + 1;
        result.expectTrue(
            synchronized && staged.accepted() && core.currentSequence().value() == expectedSequence && delivered &&
                delivered->fromSequence == frontend::SequenceNumber{expectedSequence} &&
                delivered->toSequence == frontend::SequenceNumber{expectedSequence} && delivered->events.size() == 1 &&
                delivered->events.front().type == "backend.lifecycle.changed" &&
                delivered->events.front().data.value("lifecycle", std::string{}) == "ready",
            "one observer callback retains one dispatch scope, so an inline scheduler coalesces the same key to its latest value");
    }

    void testTerminalItemKeepsInitialUpsertOrder(tests::support::TestResult& result) {
        Backend backend;
        std::vector<std::function<void()>> scheduled;
        server::ServerCoreOptions options;
        options.authenticator = authenticate;
        options.scheduler = [&scheduled](std::function<void()> callback) {
            scheduled.push_back(std::move(callback));
        };
        server::ServerCore core(backend, std::move(options));
        core.start();

        std::vector<frontend::ServerMessage> messages;
        const auto connection = core.openConnection({}, collect(messages));
        frontend::Hello hello;
        hello.capabilities = {frontend::FrontendCapability::CompleteThreadItems,
                              frontend::FrontendCapability::DedicatedNotificationEvents};
        const bool synchronized = connection && core.receive(*connection, frontend::ClientMessage{std::move(hello)}).accepted();
        drainAll(scheduled);
        messages.clear();

        server::OccurrenceCoalescingKey itemKey;
        itemKey.kind = server::OccurrenceEntityKind::Item;
        itemKey.threadId = model::ThreadIdentity{"thread-order"};
        itemKey.turnId = model::TurnIdentity{"turn-order"};
        itemKey.itemId = model::ItemIdentity{"item-order"};
        server::OccurrenceCoalescingKey contentKey = itemKey;
        contentKey.kind = server::OccurrenceEntityKind::ItemContent;
        contentKey.channel = "agentText";

        std::vector<server::OccurrenceStageRequest> groups;
        groups.push_back({itemKey, itemOccurrence("1001", "inProgress", ""), server::OccurrenceFlushUrgency::Deferred});
        groups.push_back(
            {std::move(contentKey), orderedItemContentOccurrence(), server::OccurrenceFlushUrgency::Deferred});
        groups.push_back(
            {std::move(itemKey), itemOccurrence("1003", "completed", "final text"), server::OccurrenceFlushUrgency::Immediate});
        const server::OccurrenceStageResult staged = core.stageGroups(std::move(groups));
        drainAll(scheduled);

        const auto* batch = messages.size() == 1 ? std::get_if<frontend::EventBatch>(&messages.front()) : nullptr;
        std::string observed = "messages=" + std::to_string(messages.size()) +
                               " synchronized=" + std::to_string(synchronized) +
                               " staged=" + std::to_string(staged.accepted()) +
                               " open=" + std::to_string(connection && core.connectionOpen(*connection));
        if (batch) {
            for (const frontend::FrontendEvent& event : batch->events) {
                observed += " " + event.type;
            }
        }
        result.expectTrue(synchronized && staged.accepted() && batch && batch->events.size() == 2 &&
                              batch->events[0].type == "item.upserted" && batch->events[1].type == "item.content.updated",
                          "a coalesced terminal item cannot overtake content that depends on its first upsert (" + observed + ")");

        messages.clear();
        server::OccurrenceCoalescingKey reversedItemKey;
        reversedItemKey.kind = server::OccurrenceEntityKind::Item;
        reversedItemKey.threadId = model::ThreadIdentity{"thread-order"};
        reversedItemKey.turnId = model::TurnIdentity{"turn-order"};
        reversedItemKey.itemId = model::ItemIdentity{"item-order"};
        server::OccurrenceCoalescingKey reversedContentKey = reversedItemKey;
        reversedContentKey.kind = server::OccurrenceEntityKind::ItemContent;
        reversedContentKey.channel = "agentText";
        std::vector<server::OccurrenceStageRequest> reversedGroups;
        reversedGroups.push_back(
            {std::move(reversedContentKey), orderedItemContentOccurrence(), server::OccurrenceFlushUrgency::Deferred});
        reversedGroups.push_back(
            {std::move(reversedItemKey), itemOccurrence("1004", "completed", "final text"), server::OccurrenceFlushUrgency::Immediate});
        const server::OccurrenceStageResult reversedStaged = core.stageGroups(std::move(reversedGroups));
        drainAll(scheduled);
        const auto* reversedBatch = messages.size() == 1 ? std::get_if<frontend::EventBatch>(&messages.front()) : nullptr;
        result.expectTrue(reversedStaged.accepted() && reversedBatch && reversedBatch->events.size() == 2 &&
                              reversedBatch->events[0].type == "item.upserted" &&
                              reversedBatch->events[1].type == "item.content.updated",
                          "a dependent content update is emitted after its item even when the backend reports content first");
    }

    void testIndependentSnapshotCapabilities(tests::support::TestResult& result) {
        Backend backend;
        backend.state.backendCursor.backendRevision = 9;
        backend.state.items.emplace_back(model::PlanItem{model::ItemData{model::ItemIdentity{"item-capability"}}});
        backend.state.pendingRequests.emplace_back(
            model::CommandExecutionApprovalRequest{model::PendingRequestData{model::PendingRequestIdentity{"8"}}});
        server::ServerCoreOptions options;
        options.authenticator = authenticate;
        server::ServerCore core(backend, std::move(options));
        core.start();

        const auto synchronize = [&](std::vector<frontend::FrontendCapability> capabilities,
                                     std::vector<frontend::ServerMessage>& messages) {
            const auto connection = core.openConnection({}, collect(messages));
            frontend::Hello hello;
            hello.capabilities = std::move(capabilities);
            return connection && core.receive(*connection, frontend::ClientMessage{std::move(hello)}).accepted();
        };
        std::vector<frontend::ServerMessage> legacyMessages;
        std::vector<frontend::ServerMessage> domainsMessages;
        std::vector<frontend::ServerMessage> itemsMessages;
        std::vector<frontend::ServerMessage> pendingMessages;
        std::vector<frontend::ServerMessage> allMessages;
        const bool synchronized = synchronize({}, legacyMessages) &&
                                  synchronize({frontend::FrontendCapability::CompleteBackendDomains}, domainsMessages) &&
                                  synchronize({frontend::FrontendCapability::CompleteThreadItems}, itemsMessages) &&
                                  synchronize({frontend::FrontendCapability::DedicatedPendingRequests}, pendingMessages) &&
                                  synchronize({frontend::FrontendCapability::CompleteBackendDomains,
                                               frontend::FrontendCapability::CompleteThreadItems,
                                               frontend::FrontendCapability::DedicatedPendingRequests},
                                              allMessages);
        const auto snapshotState = [](const std::vector<frontend::ServerMessage>& messages) -> const frontend::Json* {
            const auto* snapshot = messages.size() > 1 ? std::get_if<frontend::Snapshot>(&messages[1]) : nullptr;
            return snapshot ? &snapshot->state : nullptr;
        };
        const frontend::Json* legacy = snapshotState(legacyMessages);
        const frontend::Json* domains = snapshotState(domainsMessages);
        const frontend::Json* items = snapshotState(itemsMessages);
        const frontend::Json* pending = snapshotState(pendingMessages);
        const frontend::Json* all = snapshotState(allMessages);
        const bool legacyOnly = legacy && legacy->contains("backendRevision") && !legacy->contains("items") &&
                                !legacy->at("pendingRequests").at(0).contains("kind");
        const bool domainsOnly = domains && domains->contains("provider") && domains->at("items").at(0).contains("codexType") &&
                                 domains->contains("pendingRequests") && !domains->at("pendingRequests").at(0).contains("kind");
        const bool itemsOnly = items && items->contains("backendRevision") && items->at("items").at(0).contains("type") &&
                               !items->at("pendingRequests").at(0).contains("kind");
        const bool pendingOnly = pending && pending->contains("backendRevision") && !pending->contains("items") &&
                                 pending->at("pendingRequests").at(0).contains("kind");
        const bool allExpanded = all && all->contains("provider") && all->at("items").at(0).contains("type") &&
                                 all->at("pendingRequests").at(0).contains("kind");
        result.expectTrue(synchronized && legacyOnly && domainsOnly && itemsOnly && pendingOnly && allExpanded,
                          "complete domains, complete ThreadItems, and dedicated pending requests compose independently in snapshots");
    }

    void testLegacyOnlyPerConnectionContainment(tests::support::TestResult& result) {
        Backend backend;
        backend.state.provider.lifecycle = model::ProviderLifecycle::Ready;
        backend.state.threads.emplace_back(model::ThreadIdentity{"future-live-thread"});
        backend.state.turns.emplace_back(model::TurnIdentity{"future-live-turn"}, model::ThreadIdentity{"future-live-thread"});
        backend.state.legacyItems.push_back(*legacyItemOccurrence().legacyCompatibility.legacyItem);
        backend.state.legacyPendingRequests.push_back(*legacyPendingOccurrence().legacyCompatibility.legacyPendingRequest);
        server::ServerCoreOptions options;
        options.authenticator = authenticate;
        server::ServerCore core(backend, std::move(options));
        core.start();

        std::vector<frontend::ServerMessage> legacyMessages;
        std::vector<frontend::ServerMessage> expandedMessages;
        const auto legacy = core.openConnection({}, collect(legacyMessages));
        const auto expanded = core.openConnection({}, collect(expandedMessages));
        frontend::Hello legacyHello;
        frontend::Hello expandedHello;
        expandedHello.capabilities = {frontend::FrontendCapability::CompleteThreadItems,
                                      frontend::FrontendCapability::DedicatedPendingRequests};
        const bool ready = legacy && expanded && core.receive(*legacy, frontend::ClientMessage{std::move(legacyHello)}).accepted() &&
                           core.receive(*expanded, frontend::ClientMessage{std::move(expandedHello)}).accepted();
        legacyMessages.clear();
        expandedMessages.clear();

        const auto itemPublished = core.publishGroup(legacyItemOccurrence());
        core.flush();
        const auto* legacyItemBatch = legacyMessages.size() == 1 ? std::get_if<frontend::EventBatch>(&legacyMessages.front()) : nullptr;
        const auto* expandedItemSnapshot =
            expandedMessages.size() == 1 ? std::get_if<frontend::Snapshot>(&expandedMessages.front()) : nullptr;
        const bool itemSplit = itemPublished.accepted && legacyItemBatch && legacyItemBatch->events.size() == 1 &&
                               legacyItemBatch->events.front().type == "item.updated" && expandedItemSnapshot &&
                               expandedItemSnapshot->sequence == legacyItemBatch->toSequence;

        legacyMessages.clear();
        expandedMessages.clear();
        const auto pendingPublished = core.publishGroup(legacyPendingOccurrence());
        core.flush();
        const auto* legacyPendingBatch = legacyMessages.size() == 1 ? std::get_if<frontend::EventBatch>(&legacyMessages.front()) : nullptr;
        const auto* expandedPendingSnapshot =
            expandedMessages.size() == 1 ? std::get_if<frontend::Snapshot>(&expandedMessages.front()) : nullptr;
        const bool pendingSplit = pendingPublished.accepted && legacyPendingBatch && legacyPendingBatch->events.size() == 1 &&
                                  legacyPendingBatch->events.front().type == "request.pending" && expandedPendingSnapshot &&
                                  expandedPendingSnapshot->sequence == legacyPendingBatch->toSequence;

        std::vector<frontend::ServerMessage> legacyReplayMessages;
        std::vector<frontend::ServerMessage> expandedReplayMessages;
        const auto legacyReplay = core.openConnection({}, collect(legacyReplayMessages));
        const auto expandedReplay = core.openConnection({}, collect(expandedReplayMessages));
        frontend::Hello legacyReplayHello;
        legacyReplayHello.resumeAfter = frontend::SequenceNumber{0};
        frontend::Hello expandedReplayHello;
        expandedReplayHello.resumeAfter = frontend::SequenceNumber{0};
        expandedReplayHello.capabilities = {frontend::FrontendCapability::CompleteThreadItems,
                                            frontend::FrontendCapability::DedicatedPendingRequests};
        const bool replayAccepted = legacyReplay && expandedReplay &&
                                    core.receive(*legacyReplay, frontend::ClientMessage{std::move(legacyReplayHello)}).accepted() &&
                                    core.receive(*expandedReplay, frontend::ClientMessage{std::move(expandedReplayHello)}).accepted();
        const auto* legacyWelcome =
            !legacyReplayMessages.empty() ? std::get_if<frontend::Welcome>(&legacyReplayMessages.front()) : nullptr;
        const auto* expandedWelcome =
            !expandedReplayMessages.empty() ? std::get_if<frontend::Welcome>(&expandedReplayMessages.front()) : nullptr;
        const bool replaySplit = replayAccepted && legacyWelcome && legacyWelcome->syncMode == frontend::SyncMode::Replay &&
                                 expandedWelcome && expandedWelcome->syncMode == frontend::SyncMode::Snapshot &&
                                 std::any_of(legacyReplayMessages.begin(), legacyReplayMessages.end(), [](const frontend::ServerMessage& message) {
                                     const auto* batch = std::get_if<frontend::EventBatch>(&message);
                                     return batch && std::any_of(batch->events.begin(), batch->events.end(), [](const frontend::FrontendEvent& event) {
                                                return event.type == "item.updated" || event.type == "request.pending";
                                            });
                                 }) &&
                                 std::any_of(expandedReplayMessages.begin(), expandedReplayMessages.end(), [](const frontend::ServerMessage& message) {
                                     return std::holds_alternative<frontend::Snapshot>(message);
                                 });
        result.expectTrue(ready && itemSplit && pendingSplit && replaySplit,
                          "legacy-only item and pending live/replay use legacy events while expanded connections receive same-sequence Snapshot containment");
    }

    void testSessionAndControllerJournal(tests::support::TestResult& result) {
        Backend backend;
        std::vector<std::function<void()>> scheduled;
        server::ServerCoreOptions options;
        options.authenticator = [](const frontend::FrontendPeerContext&, const frontend::AuthenticationCredential&) {
            frontend::FrontendPrincipal principal;
            principal.id = "topology-principal";
            principal.profile = "test";
            principal.scopes.assign(frontend::LocalTrustedScopes.begin(), frontend::LocalTrustedScopes.end());
            return frontend::AuthenticationResult{frontend::AuthenticationSuccess{std::move(principal)}};
        };
        options.maxInboundBurst = 1000;
        options.scheduler = [&scheduled](std::function<void()> callback) {
            scheduled.push_back(std::move(callback));
        };
        server::ServerCore core(backend, std::move(options));
        core.start();

        std::vector<frontend::ServerMessage> firstMessages;
        std::vector<frontend::ServerMessage> secondMessages;
        const auto first = core.openConnection({}, collect(firstMessages));
        frontend::Hello firstHello;
        firstHello.capabilities = std::vector{frontend::FrontendCapability::CompleteBackendDomains,
                                              frontend::FrontendCapability::DedicatedNotificationEvents};
        const bool firstAccepted = first && core.receive(*first, frontend::ClientMessage{std::move(firstHello)}).accepted();
        if (!scheduled.empty()) {
            drainOne(scheduled);
        }
        firstMessages.clear();

        const auto second = core.openConnection({}, collect(secondMessages));
        frontend::Hello secondHello;
        secondHello.capabilities = std::vector{frontend::FrontendCapability::CompleteBackendDomains,
                                               frontend::FrontendCapability::DedicatedNotificationEvents};
        const bool secondAccepted = second && core.receive(*second, frontend::ClientMessage{std::move(secondHello)}).accepted();
        if (!scheduled.empty()) {
            drainOne(scheduled);
        }
        const auto* secondSnapshot = secondMessages.size() > 1 ? std::get_if<frontend::Snapshot>(&secondMessages[1]) : nullptr;
        const frontend::Json* sessions = secondSnapshot && secondSnapshot->state.contains("sessions")
                                             ? &secondSnapshot->state.at("sessions")
                                             : nullptr;
        result.expectTrue(firstAccepted && secondAccepted && core.authenticatedConnectionCount() == 2 && sessions &&
                              sessions->is_array() && sessions->size() == 2 &&
                              sessions->at(0).at("sessionId") != sessions->at(1).at("sessionId"),
                          "two observers receive distinct core-owned sessions in the canonical initial snapshot");

        const model::FrontendSequence beforeController = core.currentSequence();
        firstMessages.clear();
        secondMessages.clear();
        generated::DefinedCommand acquire{
            "topology-acquire", generated::makeParameters(generated::MethodId::ControllerAcquire, frontend::Json::object())};
        const bool acquired = first && core.receiveDefinedCommand(*first, acquire).accepted();
        if (!scheduled.empty()) {
            drainOne(scheduled);
        }
        const auto* acquiredBatch = !secondMessages.empty() ? std::get_if<frontend::EventBatch>(&secondMessages.front()) : nullptr;
        result.expectTrue(acquired && acquiredBatch && acquiredBatch->events.size() == 1 &&
                              acquiredBatch->events.front().type == "controller.updated" &&
                              core.currentSequence() == model::FrontendSequence{beforeController.value() + 1},
                          "controller acquisition advances the shared typed journal and is delivered live to the other observer");

        secondMessages.clear();
        if (first) {
            core.closeConnection(*first, "topology disconnect");
        }
        if (!scheduled.empty()) {
            drainOne(scheduled);
        }
        const auto* disconnectBatch = !secondMessages.empty() ? std::get_if<frontend::EventBatch>(&secondMessages.front()) : nullptr;
        result.expectTrue(!core.currentController() && disconnectBatch && disconnectBatch->events.size() == 2 &&
                              disconnectBatch->events[0].type == "controller.updated" &&
                              disconnectBatch->events[1].type == "sessions.updated" &&
                              core.currentSequence() == model::FrontendSequence{beforeController.value() + 3},
                          "controller disconnect publishes controller release before session removal without a parallel sequence source");

        secondMessages.clear();
        generated::DefinedCommand replay{
            "topology-replay",
            generated::makeParameters(generated::MethodId::EventsReplay, frontend::Json{{"after", beforeController.value()}})};
        const bool replayed = second && core.receiveDefinedCommand(*second, replay).accepted();
        if (!scheduled.empty()) {
            drainOne(scheduled);
        }
        const auto* replayBatch = secondMessages.size() > 1 ? std::get_if<frontend::EventBatch>(&secondMessages[1]) : nullptr;
        result.expectTrue(replayed && replayBatch && replayBatch->events.size() == 3 &&
                              replayBatch->events[0].type == "controller.updated" &&
                              replayBatch->events[1].type == "controller.updated" &&
                              replayBatch->events[2].type == "sessions.updated" &&
                              std::holds_alternative<frontend::SyncComplete>(secondMessages.back()),
                          "controller acquisition, implicit release, and session removal replay in the exact live sequence order");
    }

    backend::Snapshot entityProjectionSnapshot() {
        backend::Snapshot snapshot;
        snapshot.sequence = backend::SequenceNumber{40};
        snapshot.provider.lifecycle = backend::ProviderLifecycle::Ready;
        for (const std::string suffix : {"a", "b"}) {
            backend::ThreadSnapshot thread;
            thread.id = "thread-" + suffix;
            thread.fullyLoaded = true;

            backend::TurnSnapshot firstTurn;
            firstTurn.id = "turn-" + suffix;
            firstTurn.threadId = thread.id;
            firstTurn.status = "inProgress";
            for (const std::string itemSuffix : {"1", "2"}) {
                backend::ItemSnapshot item;
                item.id = "item-" + suffix + "-" + itemSuffix;
                item.type = "plan";
                item.status = "inProgress";
                item.agentText = "content-" + suffix + "-" + itemSuffix;
                firstTurn.items.push_back(std::move(item));
            }
            thread.turns.push_back(std::move(firstTurn));

            backend::TurnSnapshot secondTurn;
            secondTurn.id = "turn-" + suffix + "-2";
            secondTurn.threadId = thread.id;
            secondTurn.status = "inProgress";
            backend::ItemSnapshot secondTurnItem;
            secondTurnItem.id = "item-" + suffix + "-3";
            secondTurnItem.type = "plan";
            secondTurnItem.status = "inProgress";
            secondTurn.items.push_back(std::move(secondTurnItem));
            thread.turns.push_back(std::move(secondTurn));
            snapshot.threads.push_back(std::move(thread));
        }

        for (const std::string suffix : {"a", "b"}) {
            backend::ProcessSnapshot process;
            process.processHandle = "process-" + suffix;
            process.lifecycle = "running";
            snapshot.processes.push_back(std::move(process));

            backend::FilesystemWatchSnapshot watch;
            watch.watchId = "watch-" + suffix;
            snapshot.filesystemWatches.push_back(std::move(watch));

            backend::FuzzySearchSnapshot search;
            search.sessionId = "search-" + suffix;
            snapshot.fuzzySearchSessions.push_back(std::move(search));

            backend::ActivitySnapshot activity;
            activity.key = "activity-" + suffix;
            activity.subjectId = "subject-" + suffix;
            activity.kind = "test";
            activity.lifecycle = "active";
            snapshot.activities.push_back(std::move(activity));
        }
        return snapshot;
    }

    model::ModelResult<server::ProjectedBackendBatch> projectExtension(server::BackendProjection& projection,
                                                                       const backend::Snapshot& snapshot,
                                                                       std::string method,
                                                                       frontend::Json payload) {
        backend::CodexExtensionReceived extension;
        extension.method = std::move(method);
        extension.payload = std::move(payload);
        extension.safeProjection = true;
        const std::vector<backend::SequencedBackendEvent> events{
            {backend::SequenceNumber{41}, std::move(extension)}};
        return projection.projectOccurrences(events, snapshot);
    }

    bool snapshotContained(const model::ModelResult<server::ProjectedBackendBatch>& projection) {
        return projection && projection.value().snapshotRequired && projection.value().occurrences.empty();
    }

    template <typename Payload>
    const Payload* solePayload(const model::ModelResult<server::ProjectedBackendBatch>& projection) {
        if (!projection || projection.value().snapshotRequired || projection.value().occurrences.size() != 1 ||
            projection.value().occurrences.front().occurrence.expandedPayloads.size() != 1) {
            return nullptr;
        }
        return std::get_if<Payload>(&projection.value().occurrences.front().occurrence.expandedPayloads.front());
    }

    void testExactBackendOccurrenceIdentity(tests::support::TestResult& result) {
        server::BackendProjection projection;
        const backend::Snapshot source = entityProjectionSnapshot();
        const std::string malformed(model::ThreadIdentity::MaximumBytes + 1, 'x');

        const auto missingThread = projectExtension(projection, source, "thread/status/changed", frontend::Json::object());
        const auto malformedThread =
            projectExtension(projection, source, "thread/status/changed", frontend::Json{{"threadId", malformed}});
        backend::Snapshot duplicateThreadSource = source;
        duplicateThreadSource.threads.push_back(source.threads.front());
        const auto duplicateThread = projectExtension(
            projection, duplicateThreadSource, "thread/status/changed", frontend::Json{{"threadId", "thread-a"}});
        result.expectTrue(snapshotContained(missingThread),
                          "a missing thread identity requires Snapshot containment and never selects the last retained thread");
        result.expectTrue(snapshotContained(malformedThread),
                          "an unparseable thread identity requires Snapshot containment and never becomes a wildcard");
        result.expectTrue(snapshotContained(duplicateThread),
                          "an ambiguous duplicate thread identity requires Snapshot containment and never selects the first match");

        const auto exactRemoved = projectExtension(
            projection, source, "thread/deleted", frontend::Json{{"threadId", "thread-a"}});
        const auto* removed = solePayload<model::ThreadRemovedOccurrence>(exactRemoved);
        const auto missingRemoved = projectExtension(projection, source, "thread/deleted", frontend::Json::object());
        const auto malformedRemoved =
            projectExtension(projection, source, "thread/deleted", frontend::Json{{"threadId", malformed}});
        const auto conflictingRemoved = projectExtension(projection,
                                                         source,
                                                         "thread/deleted",
                                                         frontend::Json{{"threadId", "thread-a"},
                                                                        {"thread", {{"id", "thread-b"}}}});
        const auto incompleteRemoved = projectExtension(projection,
                                                        source,
                                                        "thread/deleted",
                                                        frontend::Json{{"threadId", "thread-a"}, {"thread", frontend::Json::object()}});
        const auto malformedRemovedWrapper = projectExtension(
            projection,
            source,
            "thread/deleted",
            frontend::Json{{"threadId", "thread-a"}, {"thread", frontend::Json::array()}});
        result.expectTrue(removed && removed->threadId == model::ThreadIdentity{"thread-a"},
                          "thread.removed carries the exact valid thread identity without requiring the removed entity to remain retained");
        result.expectTrue(snapshotContained(missingRemoved) && snapshotContained(malformedRemoved) &&
                              snapshotContained(conflictingRemoved) && snapshotContained(incompleteRemoved) &&
                              snapshotContained(malformedRemovedWrapper),
                          "thread.removed contains missing, malformed, conflicting, incomplete, and malformed-wrapper identities");

        const auto exactNestedTurn = projectExtension(projection,
                                                      source,
                                                      "turn/completed",
                                                      frontend::Json{{"threadId", "thread-a"},
                                                                     {"turn", {{"id", "turn-a-2"}, {"threadId", "thread-a"}}}});
        const auto* exactTurn = solePayload<model::TurnUpsertedOccurrence>(exactNestedTurn);
        result.expectTrue(exactTurn && exactTurn->turn.id == model::TurnIdentity{"turn-a-2"} &&
                              exactTurn->turn.threadId == model::ThreadIdentity{"thread-a"},
                          "a nested turn identity selects that exact turn rather than the first turn under its parent");

        const auto missingTurn =
            projectExtension(projection, source, "thread/tokenUsage/updated", frontend::Json{{"threadId", "thread-a"}});
        const auto malformedTurn = projectExtension(projection,
                                                    source,
                                                    "thread/tokenUsage/updated",
                                                    frontend::Json{{"threadId", "thread-a"}, {"turnId", malformed}});
        const auto mismatchingTurn = projectExtension(projection,
                                                      source,
                                                      "thread/tokenUsage/updated",
                                                      frontend::Json{{"threadId", "thread-b"}, {"turnId", "turn-a"}});
        backend::Snapshot duplicateTurnSource = source;
        duplicateTurnSource.threads.front().turns.push_back(source.threads.front().turns.front());
        const auto duplicateTurn = projectExtension(projection,
                                                    duplicateTurnSource,
                                                    "thread/tokenUsage/updated",
                                                    frontend::Json{{"threadId", "thread-a"}, {"turnId", "turn-a"}});
        const auto incompleteTurn = projectExtension(projection,
                                                     source,
                                                     "thread/tokenUsage/updated",
                                                     frontend::Json{{"threadId", "thread-a"},
                                                                    {"turnId", "turn-a"},
                                                                    {"turn", frontend::Json::object()}});
        result.expectTrue(snapshotContained(missingTurn),
                          "a missing turn identity requires Snapshot containment and never selects the first turn under a thread");
        result.expectTrue(snapshotContained(malformedTurn),
                          "an unparseable turn identity requires Snapshot containment and never becomes a wildcard");
        result.expectTrue(snapshotContained(mismatchingTurn),
                          "a turn identity with mismatching parent thread requires Snapshot containment");
        result.expectTrue(snapshotContained(duplicateTurn),
                          "an ambiguous duplicate thread/turn tuple requires Snapshot containment");
        result.expectTrue(snapshotContained(incompleteTurn),
                          "a present but incomplete nested turn identity cannot be treated as an absent optional wrapper");

        const auto exactNestedItem = projectExtension(projection,
                                                      source,
                                                      "item/started",
                                                      frontend::Json{{"threadId", "thread-a"},
                                                                     {"turnId", "turn-a"},
                                                                     {"item", {{"id", "item-a-2"}, {"type", "plan"}}}});
        const auto* exactItem = solePayload<model::ItemUpsertedOccurrence>(exactNestedItem);
        result.expectTrue(exactItem && model::itemData(exactItem->item).id == model::ItemIdentity{"item-a-2"},
                          "a nested item identity selects that exact item rather than the first item under its parents");

        const auto missingItem = projectExtension(projection,
                                                  source,
                                                  "item/fileChange/patchUpdated",
                                                  frontend::Json{{"threadId", "thread-a"}, {"turnId", "turn-a"}});
        const auto malformedItem = projectExtension(projection,
                                                    source,
                                                    "item/fileChange/patchUpdated",
                                                    frontend::Json{{"threadId", "thread-a"},
                                                                   {"turnId", "turn-a"},
                                                                   {"itemId", malformed}});
        const auto mismatchingItem = projectExtension(projection,
                                                      source,
                                                      "item/fileChange/patchUpdated",
                                                      frontend::Json{{"threadId", "thread-b"},
                                                                     {"turnId", "turn-b"},
                                                                     {"itemId", "item-a-1"}});
        backend::Snapshot duplicateItemSource = source;
        duplicateItemSource.threads.front().turns.front().items.push_back(source.threads.front().turns.front().items.front());
        const auto duplicateItem = projectExtension(projection,
                                                    duplicateItemSource,
                                                    "item/fileChange/patchUpdated",
                                                    frontend::Json{{"threadId", "thread-a"},
                                                                   {"turnId", "turn-a"},
                                                                   {"itemId", "item-a-1"}});
        const auto incompleteItem = projectExtension(projection,
                                                     source,
                                                     "item/fileChange/patchUpdated",
                                                     frontend::Json{{"threadId", "thread-a"},
                                                                    {"turnId", "turn-a"},
                                                                    {"itemId", "item-a-1"},
                                                                    {"item", frontend::Json::object()}});
        result.expectTrue(snapshotContained(missingItem),
                          "a missing item identity requires Snapshot containment and never selects the first item under its parents");
        result.expectTrue(snapshotContained(malformedItem),
                          "an unparseable item identity requires Snapshot containment and never becomes a wildcard");
        result.expectTrue(snapshotContained(mismatchingItem),
                          "an item identity with mismatching thread and turn parents requires Snapshot containment");
        result.expectTrue(snapshotContained(duplicateItem),
                          "an ambiguous duplicate thread/turn/item tuple requires Snapshot containment");
        result.expectTrue(snapshotContained(incompleteItem),
                          "a present but incomplete nested item identity cannot be treated as an absent optional wrapper");

        const auto exactContent = projectExtension(projection,
                                                   source,
                                                   "item/agentMessage/delta",
                                                   frontend::Json{{"threadId", "thread-a"},
                                                                  {"turnId", "turn-a"},
                                                                  {"itemId", "item-a-2"}});
        const auto* selectedContent = solePayload<model::ItemContentUpdatedOccurrence>(exactContent);
        const auto missingContent = projectExtension(projection,
                                                     source,
                                                     "item/agentMessage/delta",
                                                     frontend::Json{{"threadId", "thread-a"}, {"turnId", "turn-a"}});
        const auto malformedContent = projectExtension(projection,
                                                       source,
                                                       "item/agentMessage/delta",
                                                       frontend::Json{{"threadId", "thread-a"},
                                                                      {"turnId", "turn-a"},
                                                                      {"itemId", malformed}});
        const auto mismatchingContent = projectExtension(projection,
                                                         source,
                                                         "item/agentMessage/delta",
                                                         frontend::Json{{"threadId", "thread-b"},
                                                                        {"turnId", "turn-b"},
                                                                        {"itemId", "item-a-2"}});
        const auto incompleteContent = projectExtension(projection,
                                                        source,
                                                        "item/agentMessage/delta",
                                                        frontend::Json{{"threadId", "thread-a"},
                                                                       {"turnId", "turn-a"},
                                                                       {"itemId", "item-a-2"},
                                                                       {"item", frontend::Json::object()}});
        const auto conflictingContentParent = projectExtension(
            projection,
            source,
            "item/agentMessage/delta",
            frontend::Json{{"threadId", "thread-a"},
                           {"turnId", "turn-a"},
                           {"itemId", "item-a-2"},
                           {"item", {{"id", "item-a-2"}, {"threadId", "thread-b"}, {"turnId", "turn-a"}}}});
        result.expectTrue(selectedContent && selectedContent->itemId == model::ItemIdentity{"item-a-2"} &&
                              selectedContent->threadId == model::ThreadIdentity{"thread-a"} &&
                              selectedContent->turnId == model::TurnIdentity{"turn-a"} &&
                              selectedContent->channel == std::optional<std::string>{"agentText"} &&
                              selectedContent->content == std::optional<std::string>{"content-a-2"},
                          "item.content.updated projects the exact item, parent identities, supported channel, and canonical content");
        result.expectTrue(snapshotContained(missingContent) && snapshotContained(malformedContent) &&
                              snapshotContained(mismatchingContent) && snapshotContained(incompleteContent) &&
                              snapshotContained(conflictingContentParent),
                          "item.content.updated contains missing, malformed, parent-mismatched, and incomplete-wrapper identities");

        const auto missingProcess = projectExtension(projection, source, "process/outputDelta", frontend::Json::object());
        const auto malformedProcess =
            projectExtension(projection, source, "process/outputDelta", frontend::Json{{"processHandle", malformed}});
        backend::Snapshot duplicateProcessSource = source;
        duplicateProcessSource.processes.push_back(source.processes.front());
        const auto duplicateProcess = projectExtension(
            projection, duplicateProcessSource, "process/outputDelta", frontend::Json{{"processHandle", "process-a"}});
        const auto exactCommandProcess =
            projectExtension(projection, source, "command/exec/outputDelta", frontend::Json{{"processId", "process-b"}});
        const auto* commandProcess = solePayload<model::ProcessUpdatedOccurrence>(exactCommandProcess);
        result.expectTrue(snapshotContained(missingProcess),
                          "a missing process handle requires Snapshot containment and never selects the first process");
        result.expectTrue(snapshotContained(malformedProcess),
                          "an unparseable process handle requires Snapshot containment and never becomes a wildcard");
        result.expectTrue(snapshotContained(duplicateProcess),
                          "an ambiguous duplicate process handle requires Snapshot containment");
        result.expectTrue(commandProcess && commandProcess->process.handle == model::ProcessHandle{"process-b"},
                          "command execution process events resolve their exact processId rather than the first retained process");

        const auto missingWatch = projectExtension(projection, source, "fs/changed", frontend::Json::object());
        const auto malformedWatch = projectExtension(projection, source, "fs/changed", frontend::Json{{"watchId", malformed}});
        backend::Snapshot duplicateWatchSource = source;
        duplicateWatchSource.filesystemWatches.push_back(source.filesystemWatches.front());
        const auto duplicateWatch =
            projectExtension(projection, duplicateWatchSource, "fs/changed", frontend::Json{{"watchId", "watch-a"}});
        const auto missingSearch =
            projectExtension(projection, source, "fuzzyFileSearch/sessionUpdated", frontend::Json::object());
        const auto malformedSearch =
            projectExtension(projection, source, "fuzzyFileSearch/sessionUpdated", frontend::Json{{"sessionId", malformed}});
        backend::Snapshot duplicateSearchSource = source;
        duplicateSearchSource.fuzzySearchSessions.push_back(source.fuzzySearchSessions.front());
        const auto duplicateSearch = projectExtension(
            projection, duplicateSearchSource, "fuzzyFileSearch/sessionUpdated", frontend::Json{{"sessionId", "search-a"}});
        const auto exactSearch = projectExtension(
            projection, source, "fuzzyFileSearch/sessionUpdated", frontend::Json{{"sessionId", "search-b"}});
        const auto* selectedSearch = solePayload<model::FuzzySearchUpdatedOccurrence>(exactSearch);
        result.expectTrue(snapshotContained(missingWatch),
                          "a missing filesystem-watch identity requires Snapshot containment and never selects the first watch");
        result.expectTrue(snapshotContained(malformedWatch),
                          "a malformed filesystem-watch identity requires Snapshot containment");
        result.expectTrue(snapshotContained(duplicateWatch),
                          "an ambiguous duplicate filesystem-watch identity requires Snapshot containment");
        result.expectTrue(snapshotContained(missingSearch),
                          "a missing fuzzy-search session identity requires Snapshot containment and never selects the first session");
        result.expectTrue(snapshotContained(malformedSearch),
                          "a malformed fuzzy-search session identity requires Snapshot containment");
        result.expectTrue(snapshotContained(duplicateSearch),
                          "an ambiguous duplicate fuzzy-search session identity requires Snapshot containment");
        result.expectTrue(selectedSearch && selectedSearch->fuzzySearch.sessionId == "search-b",
                          "fuzzy-search notifications resolve their exact sessionId rather than the first retained session");

        const auto missingActivity = server::BackendProjectionTestAccess::projectActivity(projection, source, std::nullopt);
        const auto malformedActivity = server::BackendProjectionTestAccess::projectActivity(
            projection, source, std::string_view{malformed.data(), malformed.size()});
        backend::Snapshot duplicateActivitySource = source;
        duplicateActivitySource.activities.push_back(source.activities.front());
        const auto duplicateActivity = server::BackendProjectionTestAccess::projectActivity(
            projection, duplicateActivitySource, std::string_view{"activity-a"});
        const auto exactActivity =
            server::BackendProjectionTestAccess::projectActivity(projection, source, std::string_view{"activity-b"});
        const auto* selectedActivity = solePayload<model::ActivityUpdatedOccurrence>(exactActivity);
        result.expectTrue(snapshotContained(missingActivity),
                          "a missing activity key requires Snapshot containment and never selects the first activity");
        result.expectTrue(snapshotContained(malformedActivity),
                          "a malformed activity key requires Snapshot containment");
        result.expectTrue(snapshotContained(duplicateActivity),
                          "an ambiguous duplicate activity key requires Snapshot containment");
        result.expectTrue(selectedActivity && selectedActivity->activity.key == "activity-b",
                          "activity projection resolves the exact activity key through its private typed-boundary test seam");
    }

    backend::Snapshot unknownItemProjectionSnapshot() {
        backend::Snapshot snapshot;
        snapshot.sequence = backend::SequenceNumber{50};
        snapshot.provider.lifecycle = backend::ProviderLifecycle::Ready;
        backend::ThreadSnapshot thread;
        thread.id = "thread-unknown-item";
        thread.fullyLoaded = true;
        backend::TurnSnapshot turn;
        turn.id = "turn-unknown-item";
        turn.threadId = thread.id;
        turn.status = "inProgress";

        backend::ItemSnapshot before;
        before.id = "known-before";
        before.type = "plan";
        before.status = "completed";
        turn.items.push_back(std::move(before));

        backend::ItemSnapshot unknown;
        unknown.id = "future-item";
        unknown.type = "future_codex_item_kind";
        unknown.status = "completed";
        unknown.agentText = "visible agent text";
        unknown.reasoningText = "visible reasoning text";
        unknown.reasoningSummary = "visible reasoning summary";
        unknown.commandOutput = "visible command output";
        unknown.startedAtMs = 101;
        unknown.completedAtMs = 202;
        unknown.connectionInvalidated = true;
        unknown.contentTruncated = false;
        unknown.droppedContentBytes = 7;
        unknown.stamp = {13, backend::Freshness::Current};
        unknown.data = frontend::Json{{OpaqueDataKey, OpaqueDataValue}};
        turn.items.push_back(std::move(unknown));

        backend::ItemSnapshot after;
        after.id = "known-after";
        after.type = "user_message";
        after.status = "completed";
        turn.items.push_back(std::move(after));
        thread.turns.push_back(std::move(turn));
        snapshot.threads.push_back(std::move(thread));

        backend::ThreadSnapshot unrelatedThread;
        unrelatedThread.id = "thread-unrelated";
        unrelatedThread.fullyLoaded = true;
        backend::TurnSnapshot unrelatedTurn;
        unrelatedTurn.id = "turn-unrelated";
        unrelatedTurn.threadId = unrelatedThread.id;
        unrelatedTurn.status = "completed";
        backend::ItemSnapshot unrelatedItem;
        unrelatedItem.id = "known-unrelated";
        unrelatedItem.type = "web_search";
        unrelatedItem.status = "completed";
        unrelatedTurn.items.push_back(std::move(unrelatedItem));
        unrelatedThread.turns.push_back(std::move(unrelatedTurn));
        snapshot.threads.push_back(std::move(unrelatedThread));

        backend::PendingRequestSnapshot pending;
        pending.id = backend::PendingRequestId{73};
        pending.type = "apply_patch_approval";
        pending.threadId = "thread-unrelated";
        snapshot.pendingRequests.push_back(std::move(pending));

        backend::ProviderResultSummarySnapshot configurationResult;
        configurationResult.method = "config/read";
        configurationResult.status = "preserved";
        configurationResult.itemCount = 3;
        configurationResult.complete = true;
        snapshot.configuration.latestResults.push_back(std::move(configurationResult));
        return snapshot;
    }

    void testUnknownBackendItemContainment(tests::support::TestResult& result) {
        server::BackendProjection projection;
        const backend::Snapshot source = unknownItemProjectionSnapshot();
        const auto snapshot = projection.projectSnapshot(source);
        bool knownNeighborsRetained = false;
        bool unrelatedStateRetained = false;
        bool exactCompatibility = false;
        bool exactOmission = false;
        bool expandedContained = false;
        bool legacyCompatible = false;
        if (snapshot) {
            knownNeighborsRetained = snapshot.value().items.size() == 3 &&
                                     model::itemData(snapshot.value().items[0]).id == model::ItemIdentity{"known-before"} &&
                                     model::itemData(snapshot.value().items[1]).id == model::ItemIdentity{"known-after"} &&
                                     model::itemData(snapshot.value().items[2]).id == model::ItemIdentity{"known-unrelated"};
            unrelatedStateRetained =
                snapshot.value().threads.size() == 2 && snapshot.value().turns.size() == 2 &&
                snapshot.value().pendingRequests.size() == 1 &&
                model::pendingRequestData(snapshot.value().pendingRequests.front()).id == model::PendingRequestIdentity{"73"} &&
                snapshot.value().configuration.state.latestResults.size() == 1 &&
                snapshot.value().configuration.state.latestResults.front().status == "preserved";
            if (snapshot.value().legacyItems.size() == 1) {
                const model::LegacyItemCompatibility& item = snapshot.value().legacyItems.front();
                exactCompatibility =
                    item.value.id == model::ItemIdentity{"future-item"} && item.value.threadId &&
                    *item.value.threadId == model::ThreadIdentity{"thread-unknown-item"} && item.value.turnId &&
                    *item.value.turnId == model::TurnIdentity{"turn-unknown-item"} && item.sourceIndex == 1 &&
                    item.value.sourceIndex == 1 && item.discriminator == "future_codex_item_kind" && item.value.status == "completed" &&
                    item.value.agentText == "visible agent text" && item.value.reasoningText == "visible reasoning text" &&
                    item.value.reasoningSummary == "visible reasoning summary" && item.value.commandOutput == "visible command output" &&
                    item.value.startedAtMs == 101 && item.value.completedAtMs == 202 && item.value.connectionInvalidated &&
                    item.value.generation == 13 && item.value.freshness == model::Freshness::Current &&
                    item.value.contentTruncated == false && item.value.droppedContentBytes == 7 && !item.value.safeDetails.has_value() &&
                    item.value.legacyExtensions.empty() && item.value.truncation.truncated && item.value.truncation.droppedBytes == 7 &&
                    item.value.truncation.omittedPaths == std::vector<std::string>{"/threads/0/turns/0/items/1/data"};
            }
            const auto encoded =
                model::encodeProjectedSnapshot(snapshot.value(), model::SnapshotRepresentationSelection{true, true, true, false});
            if (encoded) {
                const std::string serialized = encoded.value().state.dump();
                const auto& truncation = encoded.value().state.at("truncation");
                exactOmission = truncation.at("truncated") && truncation.at("omittedEntries") == 1 &&
                                truncation.at("omittedFields") == std::vector<std::string>{"/threads/0/turns/0/items/1"};
                expandedContained =
                    serialized.find("future_codex_item_kind") == std::string::npos && serialized.find(OpaqueDataKey) == std::string::npos &&
                    serialized.find(OpaqueDataValue) == std::string::npos && serialized.find("\"id\":\"future-item\"") == std::string::npos;
            }
            const auto legacy = model::encodeLegacySnapshot(snapshot.value());
            if (legacy) {
                const auto wire = frontend::Codec::encodeServer(frontend::ServerMessage{legacy.value()});
                if (wire) {
                    const std::string serialized = wire.value().dump();
                    const auto& items = wire.value().at("state").at("threads").at(0).at("turns").at(0).at("items");
                    legacyCompatible =
                        items.size() == 3 && items.at(0).at("id") == "known-before" && items.at(1).at("id") == "future-item" &&
                        items.at(1).at("type") == "future_codex_item_kind" && items.at(1).at("agentText") == "visible agent text" &&
                        items.at(1).at("reasoningText") == "visible reasoning text" &&
                        items.at(1).at("reasoningSummary") == "visible reasoning summary" &&
                        items.at(1).at("commandOutput") == "visible command output" && items.at(1).at("contentTruncated") == true &&
                        items.at(1).at("data").empty() && items.at(1).at("extensions").empty() && items.at(2).at("id") == "known-after" &&
                        wire.value().at("state").at("threads").at(1).at("turns").at(0).at("items").at(0).at("id") == "known-unrelated" &&
                        serialized.find(OpaqueDataKey) == std::string::npos && serialized.find(OpaqueDataValue) == std::string::npos;
                }
            }
        }
        result.expectTrue(snapshot && knownNeighborsRetained,
                          "an unknown retained item is omitted while known items before and after it remain in canonical order");
        result.expectTrue(snapshot && unrelatedStateRetained,
                          "unknown-item omission preserves unrelated threads, turns, pending requests, and domain results");
        result.expectTrue(snapshot && exactCompatibility,
                          "safe-looking opaque item data is discarded while exact identity, parentage, order, visible content, and "
                          "monotonic truncation remain");
        result.expectTrue(
            snapshot && exactOmission,
            "an unknown retained item advances saturated omission/truncation accounting exactly once at its deterministic path");
        result.expectTrue(snapshot && expandedContained && legacyCompatible,
                          "an unknown outer discriminator remains bounded and ordered on the legacy wire but cannot enter expanded state");

        const std::vector<backend::SequencedBackendEvent> unrelatedEvents{
            {backend::SequenceNumber{51}, backend::CapacityChanged{backend::CapacityMetric::RejectedSessions, 1}}};
        const auto unrelated = projection.projectOccurrences(unrelatedEvents, source);
        const auto* capacity = solePayload<model::CapacityUpdatedOccurrence>(unrelated);
        const auto unrelatedThreadEvent = projectExtension(
            projection, source, "thread/status/changed", frontend::Json{{"threadId", "thread-unrelated"}});
        const auto* unrelatedThread = solePayload<model::ThreadUpsertedOccurrence>(unrelatedThreadEvent);
        result.expectTrue(capacity != nullptr && unrelatedThread &&
                              unrelatedThread->thread.id == model::ThreadIdentity{"thread-unrelated"} &&
                              unrelatedThread->turns.size() == 1 && unrelatedThread->items.size() == 1 &&
                              model::itemData(unrelatedThread->items.front()).id == model::ItemIdentity{"known-unrelated"},
                          "unrelated aggregate and thread occurrences remain projectable when an unknown item is retained elsewhere");

        const auto targetedUnknown = projectExtension(projection,
                                                      source,
                                                      "item/fileChange/patchUpdated",
                                                      frontend::Json{{"threadId", "thread-unknown-item"},
                                                                     {"turnId", "turn-unknown-item"},
                                                                     {"itemId", "future-item"}});
        const bool legacyOnlyOccurrence =
            targetedUnknown && !targetedUnknown.value().snapshotRequired && targetedUnknown.value().occurrences.size() == 1 &&
            targetedUnknown.value().occurrences.front().occurrence.expandedPayloads.empty() &&
            targetedUnknown.value().occurrences.front().occurrence.legacyCompatibility.kind ==
                model::LegacyCompatibilityKind::LegacyItem &&
            targetedUnknown.value().occurrences.front().occurrence.legacyCompatibility.legacyItem.has_value() &&
            targetedUnknown.value().occurrences.front().occurrence.legacyCompatibility.legacyItem->value.id ==
                model::ItemIdentity{"future-item"};
        result.expectTrue(legacyOnlyOccurrence,
                          "an occurrence targeting a future item retains its exact legacy identity without fabricating an expanded discriminator");

        backend::Snapshot malformedKnownSource = source;
        malformedKnownSource.threads.front().turns.front().items.front().id.clear();
        const auto malformedKnown = projection.projectSnapshot(malformedKnownSource);
        result.expectTrue(!malformedKnown && malformedKnown.error().code == model::ModelErrorCode::InvalidIdentifier &&
                              malformedKnown.error().path == "/items/id",
                          "a malformed instance of a known item kind remains a strict projection error rather than an omitted future item");

        backend::Snapshot extensionSource = source;
        backend::ItemSnapshot& extensionItem = extensionSource.threads.front().turns.front().items.at(1);
        extensionItem.data = frontend::Json::object();
        extensionItem.extensions = frontend::Json{{OpaqueExtensionKey, OpaqueExtensionValue}};
        const auto extensionSnapshot = projection.projectSnapshot(extensionSource);
        bool opaqueExtensionContained = false;
        if (extensionSnapshot && extensionSnapshot.value().legacyItems.size() == 1) {
            const model::LegacyItemCompatibility& item = extensionSnapshot.value().legacyItems.front();
            const auto legacy = model::encodeLegacySnapshot(extensionSnapshot.value());
            const auto wire = legacy ? frontend::Codec::encodeServer(frontend::ServerMessage{legacy.value()})
                                     : frontend::CodecResult<frontend::Json>{frontend::CodecError{}};
            opaqueExtensionContained =
                !item.value.safeDetails.has_value() && item.value.legacyExtensions.empty() && item.value.truncation.truncated &&
                item.value.truncation.omittedPaths == std::vector<std::string>{"/threads/0/turns/0/items/1/extensions"} && wire &&
                wire.value().dump().find(OpaqueExtensionKey) == std::string::npos &&
                wire.value().dump().find(OpaqueExtensionValue) == std::string::npos;
        }
        result.expectTrue(opaqueExtensionContained,
                          "safe-looking opaque item extensions are unconditionally discarded and record their exact omission path");

        backend::Snapshot secretSource = source;
        backend::ItemSnapshot& secretItem = secretSource.threads.front().turns.front().items.at(1);
        secretItem.data = frontend::Json{{"accessToken", SecretDataValue}};
        const auto secretSnapshot = projection.projectSnapshot(secretSource);
        bool secretContained = false;
        if (secretSnapshot && secretSnapshot.value().legacyItems.size() == 1) {
            const model::LegacyItemCompatibility& item = secretSnapshot.value().legacyItems.front();
            const auto legacy = model::encodeLegacySnapshot(secretSnapshot.value());
            const auto wire = legacy ? frontend::Codec::encodeServer(frontend::ServerMessage{legacy.value()})
                                     : frontend::CodecResult<frontend::Json>{frontend::CodecError{}};
            secretContained = !item.value.safeDetails.has_value() && item.value.truncation.truncated &&
                              item.value.truncation.omittedPaths == std::vector<std::string>{"/threads/0/turns/0/items/1/data"} && wire &&
                              wire.value().dump().find("accessToken") == std::string::npos &&
                              wire.value().dump().find(SecretDataValue) == std::string::npos;
        }
        result.expectTrue(secretContained, "known secret-bearing unknown item data remains a distinct fail-closed containment case");

        backend::Snapshot utf8Source = source;
        backend::ItemSnapshot& utf8Item = utf8Source.threads.front().turns.front().items.at(1);
        utf8Item.data = frontend::Json::object();
        utf8Item.type = std::string(model::ItemIdentity::MaximumBytes - 1, 'u') + "\xE2\x82\xAC";
        const auto utf8Snapshot = projection.projectSnapshot(utf8Source);
        bool utf8Bounded = false;
        if (utf8Snapshot && utf8Snapshot.value().legacyItems.size() == 1) {
            const model::LegacyItemCompatibility& item = utf8Snapshot.value().legacyItems.front();
            const auto legacy = model::encodeLegacySnapshot(utf8Snapshot.value());
            const auto wire = legacy ? frontend::Codec::encodeServer(frontend::ServerMessage{legacy.value()})
                                     : frontend::CodecResult<frontend::Json>{frontend::CodecError{}};
            utf8Bounded = item.discriminator == std::string(model::ItemIdentity::MaximumBytes - 1, 'u') && !item.discriminator.empty() &&
                          item.discriminator.size() <= model::ItemIdentity::MaximumBytes && item.value.truncation.truncated && wire &&
                          wire.value().at("state").at("threads").at(0).at("turns").at(0).at("items").at(0).at("id") == "known-before" &&
                          wire.value().at("state").at("threads").at(0).at("turns").at(0).at("items").at(2).at("id") == "known-after";
        }
        result.expectTrue(
            utf8Bounded,
            "an oversized multibyte unknown discriminator is bounded only at a valid UTF-8 boundary without disturbing neighbors");

        backend::Snapshot emptyDiscriminatorSource = source;
        backend::ItemSnapshot& emptyDiscriminatorItem = emptyDiscriminatorSource.threads.front().turns.front().items.at(1);
        emptyDiscriminatorItem.data = frontend::Json::object();
        emptyDiscriminatorItem.type.clear();
        const auto emptyDiscriminatorSnapshot = projection.projectSnapshot(emptyDiscriminatorSource);
        backend::Snapshot invalidDiscriminatorSource = source;
        backend::ItemSnapshot& invalidDiscriminatorItem = invalidDiscriminatorSource.threads.front().turns.front().items.at(1);
        invalidDiscriminatorItem.data = frontend::Json::object();
        invalidDiscriminatorItem.type = std::string{"plan"} + static_cast<char>(0xff);
        const auto invalidDiscriminatorSnapshot = projection.projectSnapshot(invalidDiscriminatorSource);
        const bool discriminatorFallback = emptyDiscriminatorSnapshot && emptyDiscriminatorSnapshot.value().legacyItems.size() == 1 &&
                                           emptyDiscriminatorSnapshot.value().legacyItems.front().discriminator == "unknown" &&
                                           emptyDiscriminatorSnapshot.value().legacyItems.front().value.truncation.truncated &&
                                           invalidDiscriminatorSnapshot && invalidDiscriminatorSnapshot.value().legacyItems.size() == 1 &&
                                           invalidDiscriminatorSnapshot.value().legacyItems.front().discriminator == "unknown" &&
                                           invalidDiscriminatorSnapshot.value().legacyItems.front().value.truncation.truncated;
        result.expectTrue(
            discriminatorFallback,
            "empty or malformed UTF-8 future discriminators use the bounded unknown fallback instead of inventing a known kind");
    }

    bool exerciseUnknownItemWireContainment(backend::Snapshot source, std::string_view opaqueKey, std::string_view opaqueValue) {
        server::BackendProjection projection;
        const auto projectedSnapshot = projection.projectSnapshot(source);
        const auto projectedOccurrence = projectExtension(
            projection,
            source,
            "item/fileChange/patchUpdated",
            frontend::Json{{"threadId", "thread-unknown-item"}, {"turnId", "turn-unknown-item"}, {"itemId", "future-item"}});
        if (!projectedSnapshot || !projectedOccurrence || projectedOccurrence.value().snapshotRequired ||
            projectedOccurrence.value().occurrences.size() != 1) {
            return false;
        }

        Backend backend;
        backend.state = projectedSnapshot.value();
        std::vector<std::function<void()>> scheduled;
        server::ServerCoreOptions options;
        options.authenticator = authenticate;
        options.maxInboundBurst = 1000;
        options.scheduler = [&scheduled](std::function<void()> callback) {
            scheduled.push_back(std::move(callback));
        };
        server::ServerCore core(backend, std::move(options));
        core.start();

        std::vector<frontend::ServerMessage> legacyMessages;
        std::vector<frontend::ServerMessage> expandedMessages;
        const auto legacy = core.openConnection({}, collect(legacyMessages));
        const auto expanded = core.openConnection({}, collect(expandedMessages));
        frontend::Hello expandedHello;
        expandedHello.capabilities = {frontend::FrontendCapability::CompleteBackendDomains,
                                      frontend::FrontendCapability::CompleteThreadItems};
        const bool synchronized = legacy && expanded && core.receive(*legacy, frontend::ClientMessage{frontend::Hello{}}).accepted() &&
                                  core.receive(*expanded, frontend::ClientMessage{std::move(expandedHello)}).accepted();
        drainAll(scheduled);

        const auto legacySnapshot = std::find_if(legacyMessages.begin(), legacyMessages.end(), [](const frontend::ServerMessage& message) {
            return std::holds_alternative<frontend::Snapshot>(message);
        });
        const auto expandedSnapshot =
            std::find_if(expandedMessages.begin(), expandedMessages.end(), [](const frontend::ServerMessage& message) {
                return std::holds_alternative<frontend::Snapshot>(message);
            });
        bool snapshotContained = false;
        if (legacySnapshot != legacyMessages.end() && expandedSnapshot != expandedMessages.end()) {
            const frontend::Json& legacyState = std::get<frontend::Snapshot>(*legacySnapshot).state;
            const frontend::Json& unknown = legacyState.at("threads").at(0).at("turns").at(0).at("items").at(1);
            const frontend::Json& expandedState = std::get<frontend::Snapshot>(*expandedSnapshot).state;
            snapshotContained =
                unknown.at("id") == "future-item" && unknown.at("type") == "future_codex_item_kind" &&
                unknown.at("agentText") == "visible agent text" && unknown.at("contentTruncated") == true && unknown.at("data").empty() &&
                unknown.at("extensions").empty() && expandedState.at("truncation").at("omittedEntries") == 1 &&
                expandedState.at("truncation").at("omittedFields") == std::vector<std::string>{"/threads/0/turns/0/items/1"} &&
                expandedState.dump().find("future-item") == std::string::npos;
        }
        const bool snapshotOpaqueAbsent = excludesOpaqueStrings(legacyMessages, opaqueKey, opaqueValue) &&
                                          excludesOpaqueStrings(expandedMessages, opaqueKey, opaqueValue);

        legacyMessages.clear();
        expandedMessages.clear();
        const model::FrontendSequence replayAnchor = core.currentSequence();
        const server::PublishResult published = core.publishGroup(projectedOccurrence.value().occurrences.front().occurrence);
        drainAll(scheduled);
        const auto liveItem = std::find_if(legacyMessages.begin(), legacyMessages.end(), [](const frontend::ServerMessage& message) {
            const auto* batch = std::get_if<frontend::EventBatch>(&message);
            return batch && std::any_of(batch->events.begin(), batch->events.end(), [](const frontend::FrontendEvent& event) {
                       return event.type == "item.updated" && event.data.at("item").at("id") == "future-item" &&
                              event.data.at("item").at("contentTruncated") == true && event.data.at("item").at("data").empty() &&
                              event.data.at("item").at("extensions").empty();
                   });
        });
        const bool expandedLiveSnapshot =
            std::any_of(expandedMessages.begin(), expandedMessages.end(), [](const frontend::ServerMessage& message) {
                return std::holds_alternative<frontend::Snapshot>(message);
            });
        const bool liveContained = published.accepted && !published.error.has_value() &&
                                   published.deliveryMode == server::PublishDeliveryMode::Occurrences && liveItem != legacyMessages.end() &&
                                   expandedLiveSnapshot && excludesOpaqueStrings(legacyMessages, opaqueKey, opaqueValue) &&
                                   excludesOpaqueStrings(expandedMessages, opaqueKey, opaqueValue);

        std::vector<frontend::ServerMessage> legacyReplayMessages;
        std::vector<frontend::ServerMessage> expandedReplayMessages;
        const auto legacyReplay = core.openConnection({}, collect(legacyReplayMessages));
        const auto expandedReplay = core.openConnection({}, collect(expandedReplayMessages));
        frontend::Hello legacyReplayHello;
        legacyReplayHello.resumeAfter = replayAnchor.protocolValue();
        frontend::Hello expandedReplayHello;
        expandedReplayHello.resumeAfter = replayAnchor.protocolValue();
        expandedReplayHello.capabilities = {frontend::FrontendCapability::CompleteBackendDomains,
                                            frontend::FrontendCapability::CompleteThreadItems};
        const bool replayAccepted = legacyReplay && expandedReplay &&
                                    core.receive(*legacyReplay, frontend::ClientMessage{std::move(legacyReplayHello)}).accepted() &&
                                    core.receive(*expandedReplay, frontend::ClientMessage{std::move(expandedReplayHello)}).accepted();
        drainAll(scheduled);
        const auto* legacyWelcome = !legacyReplayMessages.empty() ? std::get_if<frontend::Welcome>(&legacyReplayMessages.front()) : nullptr;
        const auto* expandedWelcome =
            !expandedReplayMessages.empty() ? std::get_if<frontend::Welcome>(&expandedReplayMessages.front()) : nullptr;
        const bool replayItem =
            std::any_of(legacyReplayMessages.begin(), legacyReplayMessages.end(), [](const frontend::ServerMessage& message) {
                const auto* batch = std::get_if<frontend::EventBatch>(&message);
                return batch && std::any_of(batch->events.begin(), batch->events.end(), [](const frontend::FrontendEvent& event) {
                           return event.type == "item.updated" && event.data.at("item").at("id") == "future-item";
                       });
            });
        const bool replayContained = replayAccepted && legacyWelcome && legacyWelcome->syncMode == frontend::SyncMode::Replay &&
                                     expandedWelcome && expandedWelcome->syncMode == frontend::SyncMode::Snapshot && replayItem &&
                                     excludesOpaqueStrings(legacyReplayMessages, opaqueKey, opaqueValue) &&
                                     excludesOpaqueStrings(expandedReplayMessages, opaqueKey, opaqueValue);
        return synchronized && snapshotContained && snapshotOpaqueAbsent && liveContained && replayContained;
    }

    void testUnknownBackendItemWireContainment(tests::support::TestResult& result) {
        const backend::Snapshot dataSource = unknownItemProjectionSnapshot();
        result.expectTrue(
            exerciseUnknownItemWireContainment(dataSource, OpaqueDataKey, OpaqueDataValue),
            "safe-looking opaque unknown item data cannot escape through legacy Snapshot, live, replay, or expanded containment");

        backend::Snapshot extensionSource = unknownItemProjectionSnapshot();
        backend::ItemSnapshot& extensionItem = extensionSource.threads.front().turns.front().items.at(1);
        extensionItem.data = frontend::Json::object();
        extensionItem.extensions = frontend::Json{{OpaqueExtensionKey, OpaqueExtensionValue}};
        result.expectTrue(
            exerciseUnknownItemWireContainment(extensionSource, OpaqueExtensionKey, OpaqueExtensionValue),
            "safe-looking opaque unknown item extensions cannot escape through legacy Snapshot, live, replay, or expanded containment");
    }

    void testUnknownPendingRequestCompatibility(tests::support::TestResult& result) {
        backend::Snapshot source = unknownItemProjectionSnapshot();
        source.pendingRequests.clear();
        for (const auto& [id, type] : std::array<std::pair<std::uint64_t, std::string_view>, 3>{
                 {{71, "command_approval"}, {72, "unknown"}, {73, "file_change_approval"}}}) {
            backend::PendingRequestSnapshot pending;
            pending.id = backend::PendingRequestId{id};
            pending.type = std::string(type);
            pending.threadId = "thread-unrelated";
            if (id == 72) {
                pending.details = frontend::Json{{"method", "future/serverRequest"},
                                                 {"params", {{"safeSentinel", "future-request-safe"}}},
                                                 {"sensitiveFieldsRedacted", true}};
            }
            source.pendingRequests.push_back(std::move(pending));
        }
        server::BackendProjection projection;
        const auto snapshot = projection.projectSnapshot(source);
        bool legacyCompatible = false;
        bool expandedContained = false;
        if (snapshot) {
            const auto legacy = model::encodeLegacySnapshot(snapshot.value());
            const auto expanded = model::encodeProjectedSnapshot(
                snapshot.value(), model::SnapshotRepresentationSelection{true, false, true, false});
            if (legacy && expanded) {
                const auto& pending = legacy.value().state.at("pendingRequests");
                legacyCompatible = pending.size() == 3 && pending.at(0).at("id") == "71" && pending.at(1).at("id") == "72" &&
                                   pending.at(1).at("type") == "unknown" && pending.at(2).at("id") == "73" &&
                                   legacy.value().state.dump().find("future-request-safe") != std::string::npos;
                const std::string serialized = expanded.value().state.dump();
                const auto& truncation = expanded.value().state.at("truncation");
                expandedContained = expanded.value().state.at("pendingRequests").size() == 2 &&
                                    serialized.find("future/serverRequest") == std::string::npos && truncation.at("omittedEntries") == 1 &&
                                    truncation.at("omittedFields") == std::vector<std::string>{"/pendingRequests/1"};
            }
        }
        result.expectTrue(snapshot && legacyCompatible,
                          "a generic unknown pending request remains bounded in legacy source order without poisoning the snapshot");
        result.expectTrue(snapshot && expandedContained,
                          "dedicated pending-request projection omits and accounts one generic request without losing known neighbors");
    }

    backend::Snapshot userInputPresentationSnapshot(frontend::Json question) {
        backend::Snapshot source;
        source.sequence = backend::SequenceNumber{60};
        source.provider.lifecycle = backend::ProviderLifecycle::Ready;
        backend::PendingRequestSnapshot pending;
        pending.id = backend::PendingRequestId{80};
        pending.type = "user_input";
        pending.details = frontend::Json{{"questions", frontend::Json::array({std::move(question)})}};
        source.pendingRequests.push_back(std::move(pending));
        return source;
    }

    frontend::Json userInputPresentationQuestion(std::string id,
                                                 std::string header,
                                                 std::string prompt,
                                                 std::string optionLabel,
                                                 std::string optionDescription) {
        return frontend::Json{{"id", std::move(id)},
                              {"header", std::move(header)},
                              {"prompt", std::move(prompt)},
                              {"allowsFreeText", true},
                              {"secret", false},
                              {"options",
                               frontend::Json::array({frontend::Json{{"label", std::move(optionLabel)},
                                                                     {"description", std::move(optionDescription)}}})}};
    }

    void testPendingUserInputPresentationUtf8Containment(tests::support::TestResult& result) {
        const std::string euro = "\xE2\x82\xAC";
        const std::string expectedId = std::string(1'023, 'i') + euro;
        const std::string expectedHeader = std::string(16'383, 'h') + euro;
        const std::string expectedPrompt = std::string(16'383, 'p') + euro;
        const std::string expectedOptionLabel = std::string(16'383, 'l') + euro;
        const std::string expectedOptionDescription = std::string(16'383, 'd') + euro;
        const backend::Snapshot oversizedSource = userInputPresentationSnapshot(userInputPresentationQuestion(
            expectedId + "overflow",
            expectedHeader + "overflow",
            expectedPrompt + "overflow",
            expectedOptionLabel + "overflow",
            expectedOptionDescription + "overflow"));

        server::BackendProjection projection;
        const auto oversized = projection.projectSnapshot(oversizedSource);
        bool exactUtf8Prefixes = false;
        bool truncationRecorded = false;
        bool frontendWireEncodable = false;
        if (oversized && oversized.value().pendingRequests.size() == 1) {
            const model::PendingRequestData& pending = model::pendingRequestData(oversized.value().pendingRequests.front());
            if (pending.questions.size() == 1 && pending.questions.front().options.size() == 1) {
                const model::PendingRequestQuestion& question = pending.questions.front();
                const model::PendingRequestOption& option = question.options.front();
                exactUtf8Prefixes = question.id == expectedId && question.header == expectedHeader &&
                                    question.prompt == expectedPrompt && option.label == expectedOptionLabel &&
                                    option.description == expectedOptionDescription;
                truncationRecorded =
                    pending.truncation.truncated &&
                    pending.truncation.omittedPaths ==
                        std::vector<std::string>{"/pendingRequests/details/questions/0/id",
                                                 "/pendingRequests/details/questions/0/header",
                                                 "/pendingRequests/details/questions/0/prompt",
                                                 "/pendingRequests/details/questions/0/options/0/label",
                                                 "/pendingRequests/details/questions/0/options/0/description"};
            }
            const auto encoded = model::encodeProjectedSnapshot(
                oversized.value(), model::SnapshotRepresentationSelection{true, true, true, false});
            if (encoded) {
                frontendWireEncodable = frontend::Codec::encodeServer(frontend::ServerMessage{encoded.value()}).hasValue();
            }
        }
        result.expectTrue(oversized && exactUtf8Prefixes && truncationRecorded && frontendWireEncodable,
                          "bounded user-input id, header, prompt, option label, and option description retain only complete UTF-8 "
                          "code points and remain serializable");

        std::string isolatedContinuation(1, static_cast<char>(0x80));
        const auto malformedHeader = projection.projectSnapshot(userInputPresentationSnapshot(userInputPresentationQuestion(
            "question-1", std::move(isolatedContinuation), "prompt", "label", "description")));
        std::string truncatedMultibyteLead{"description"};
        truncatedMultibyteLead.push_back(static_cast<char>(0xE2));
        truncatedMultibyteLead.push_back(static_cast<char>(0x82));
        const auto malformedOption = projection.projectSnapshot(userInputPresentationSnapshot(userInputPresentationQuestion(
            "question-2", "header", "prompt", "label", std::move(truncatedMultibyteLead))));
        std::string malformedBeyondBound(16'384, 'h');
        malformedBeyondBound.push_back(static_cast<char>(0x80));
        const auto malformedSuffix = projection.projectSnapshot(userInputPresentationSnapshot(userInputPresentationQuestion(
            "question-3", std::move(malformedBeyondBound), "prompt", "label", "description")));
        result.expectTrue(!malformedHeader && !malformedOption && !malformedSuffix,
                          "malformed UTF-8 anywhere in pending user-input presentation is rejected before truncation, canonical "
                          "retention, or serialization");
    }

    void testMinimalSnapshotKeepsUserInputActionable(tests::support::TestResult& result) {
        backend::BackendState state;
        state.provider.lifecycle = backend::ProviderLifecycle::Ready;
        state.provider.generation = 1;
        typed::UserInputQuestion question{
            "question-1", "Choice", "Select an option", {{"One", "First option", frontend::Json::object()}}, true, false, {}};
        typed::UserInputRequest request{ai::openai::codex::ServerRequestId{std::int64_t{8}},
                                        ai::openai::codex::ServerRequestToken{8},
                                        typed::ThreadId{"thread-actionable"},
                                        typed::TurnId{"turn-actionable"},
                                        typed::ItemId{"item-actionable"},
                                        {std::move(question)},
                                        std::nullopt,
                                        frontend::Json::object(),
                                        {},
                                        {}};
        state.pendingRequests.emplace(
            backend::PendingRequestId{8},
            backend::PendingRequestState{backend::PendingRequestId{8}, typed::TypedServerRequest{std::move(request)}, 1});
        state.capacity.limits.maxSnapshotBytes = 0;

        const backend::Snapshot bounded = backend::makeSnapshot(state);
        server::BackendProjection projection;
        const auto projected = projection.projectSnapshot(bounded);
        const auto encoded = projected ? model::encodeProjectedSnapshot(
                                             projected.value(), model::SnapshotRepresentationSelection{true, true, true, false})
                                       : model::ModelResult<frontend::Snapshot>{projected.error()};
        const model::PendingRequestData* pending =
            projected && projected.value().pendingRequests.size() == 1
                ? &model::pendingRequestData(projected.value().pendingRequests.front())
                : nullptr;
        result.expectTrue(pending && pending->questionsPresent && pending->questions.size() == 1 &&
                              pending->questions.front().id == "question-1" && pending->questions.front().prompt == "Select an option" &&
                              pending->questions.front().options.size() == 1 &&
                              pending->questions.front().options.front().label == "One" && encoded &&
                              frontend::Codec::encodeServer(frontend::ServerMessage{encoded.value()}).hasValue(),
                          "minimal snapshots retain response-critical user-input questions and remain wire encodable");

        state.pendingRequests.clear();
        typed::UserInputQuestion oversizedQuestion{
            "question-2", "Choice", std::string(70U * 1024U, 'p'), {}, true, false, {}};
        typed::UserInputRequest oversizedRequest{ai::openai::codex::ServerRequestId{std::int64_t{9}},
                                                 ai::openai::codex::ServerRequestToken{9},
                                                 typed::ThreadId{"thread-actionable"},
                                                 typed::TurnId{"turn-actionable"},
                                                 typed::ItemId{"item-actionable"},
                                                 {std::move(oversizedQuestion)},
                                                 std::nullopt,
                                                 frontend::Json::object(),
                                                 {},
                                                 {}};
        state.pendingRequests.emplace(
            backend::PendingRequestId{9},
            backend::PendingRequestState{backend::PendingRequestId{9}, typed::TypedServerRequest{std::move(oversizedRequest)}, 1});
        const auto oversizedProjected = projection.projectSnapshot(backend::makeSnapshot(state));
        const auto oversizedEncoded = oversizedProjected
                                          ? model::encodeProjectedSnapshot(
                                                oversizedProjected.value(),
                                                model::SnapshotRepresentationSelection{true, true, true, false})
                                          : model::ModelResult<frontend::Snapshot>{oversizedProjected.error()};
        const model::PendingRequestData* oversizedPending =
            oversizedProjected && oversizedProjected.value().pendingRequests.size() == 1
                ? &model::pendingRequestData(oversizedProjected.value().pendingRequests.front())
                : nullptr;
        result.expectTrue(oversizedPending && oversizedPending->questionsPresent && oversizedPending->questions.empty() &&
                              oversizedPending->truncation.truncated && oversizedEncoded &&
                              frontend::Codec::encodeServer(frontend::ServerMessage{oversizedEncoded.value()}).hasValue(),
                          "oversized user-input details fail soft as explicitly truncated while preserving the wire contract");
    }

    void testBackendProjection(tests::support::TestResult& result) {
        ai::openai::codex::backend::Snapshot source;
        source.sequence = ai::openai::codex::backend::SequenceNumber{17};
        source.provider.lifecycle = ai::openai::codex::backend::ProviderLifecycle::Ready;
        source.controller = ai::openai::codex::backend::SessionId{3};
        source.sessions.push_back({ai::openai::codex::backend::SessionId{3},
                                   ai::openai::codex::backend::SessionRole::Controller});
        ai::openai::codex::backend::ThreadSnapshot thread;
        thread.id = "thread-projection";
        thread.title = "Projection thread";
        thread.fullyLoaded = true;
        ai::openai::codex::backend::TurnSnapshot turn;
        turn.id = "turn-projection";
        turn.threadId = thread.id;
        turn.status = "inProgress";
        ai::openai::codex::backend::ItemSnapshot item;
        item.id = "item-projection";
        item.type = "plan";
        item.status = "inProgress";
        item.agentText = "canonical accumulated content";
        turn.items.push_back(item);
        thread.turns.push_back(turn);
        source.threads.push_back(thread);
        ai::openai::codex::backend::PendingRequestSnapshot pending;
        pending.id = ai::openai::codex::backend::PendingRequestId{9};
        pending.type = "apply_patch_approval";
        pending.threadId = thread.id;
        pending.details = frontend::Json{{"apiKey", "must-not-enter-the-model"}};
        source.pendingRequests.push_back(std::move(pending));
        source.notices.push_back({1,
                                  ai::openai::codex::backend::NoticeCategory::Configuration,
                                  "configuration warning",
                                  std::nullopt,
                                  std::nullopt,
                                  {}});
        source.diagnostics.received = 1;
        source.diagnostics.recent.push_back("bounded diagnostic");

        server::BackendProjection projection;
        const auto snapshot = projection.projectSnapshot(source);
        const bool boundedPending = snapshot && snapshot.value().pendingRequests.size() == 1 &&
                                    model::pendingRequestData(snapshot.value().pendingRequests.front()).safeDetails &&
                                    model::pendingRequestData(snapshot.value().pendingRequests.front()).safeDetails->empty() &&
                                    snapshot.value().truncation.truncated;
        result.expectTrue(snapshot && snapshot.value().provider.ready() && snapshot.value().threads.size() == 1 &&
                              snapshot.value().turns.size() == 1 && snapshot.value().items.size() == 1 && boundedPending,
                          "backend snapshots convert once into strong identities and reject secret-bearing safe-detail extensions");

        ai::openai::codex::backend::Snapshot providerOperationSource = source;
        providerOperationSource.sequence = ai::openai::codex::backend::SequenceNumber{18};
        providerOperationSource.providerOperations.push_back({"model/list", 12, {4, ai::openai::codex::backend::Freshness::Current}});
        const std::vector<ai::openai::codex::backend::SequencedBackendEvent> providerOperationEvents{
            {ai::openai::codex::backend::SequenceNumber{18}, ai::openai::codex::backend::ProviderOperationStateChanged{"model/list"}}};
        const auto providerOperationBatch = projection.projectOccurrences(providerOperationEvents, providerOperationSource);
        const frontend::Json providerOperationSemantics =
            providerOperationBatch ? providerOperationBatch.value().snapshot.extensions.json() : frontend::Json::object();
        const frontend::Json providerOperations = providerOperationSemantics.value("providerOperationsSemantic", frontend::Json::object());
        const frontend::Json providerMethods = providerOperations.value("methods", frontend::Json::array());
        const auto providerOperationWire =
            providerOperationBatch ? model::encodeProjectedSnapshot(providerOperationBatch.value().snapshot,
                                                                    model::SnapshotRepresentationSelection{true, true, true, true})
                                   : model::ModelResult<frontend::Snapshot>{providerOperationBatch.error()};
        result.expectTrue(providerOperationBatch && providerOperationBatch.value().snapshotRequired &&
                              std::find(providerMethods.begin(), providerMethods.end(), "model/list") != providerMethods.end() &&
                              providerOperationWire,
                          "provider-operation fallback retains a complete publication-ready canonical Snapshot for direct reuse");

        ai::openai::codex::backend::Snapshot previewSource = source;
        previewSource.threads.front().preview = std::string(16'383, 'p') + "\xE2\x82\xAC";
        const auto previewSnapshot = projection.projectSnapshot(previewSource);
        const auto previewWire =
            previewSnapshot
                ? model::encodeProjectedSnapshot(previewSnapshot.value(), model::SnapshotRepresentationSelection{true, true, true, true})
                : model::ModelResult<frontend::Snapshot>{previewSnapshot.error()};
        const frontend::Json previewDetails = previewSnapshot && previewSnapshot.value().threads.size() == 1
                                                  ? previewSnapshot.value().threads.front().safeDetails.json()
                                                  : frontend::Json::object();
        result.expectTrue(previewWire && previewDetails.value("preview", std::string{}).size() == 16'386 &&
                              previewDetails.value("preview", std::string{}) == std::string(16'383, 'p') + "\xE2\x82\xAC",
                          "backend thread previews are bounded by frontend Unicode characters without splitting UTF-8");

        ai::openai::codex::backend::Snapshot oversizedItemSource = source;
        ai::openai::codex::backend::ItemSnapshot& oversizedItem =
            oversizedItemSource.threads.front().turns.front().items.front();
        oversizedItem.status = "started";
        const std::string retainedItemContent = std::string(16'383, 'x') + "\xE2\x82\xAC";
        const std::string oversizedItemContent = retainedItemContent + "tail";
        oversizedItem.agentText = oversizedItemContent;
        oversizedItem.reasoningText = oversizedItemContent;
        oversizedItem.reasoningSummary = oversizedItemContent;
        oversizedItem.commandOutput = oversizedItemContent;
        const auto oversizedItemSnapshot = projection.projectSnapshot(oversizedItemSource);
        const auto oversizedItemWire =
            oversizedItemSnapshot
                ? model::encodeProjectedSnapshot(oversizedItemSnapshot.value(),
                                                  model::SnapshotRepresentationSelection{true, true, true, true})
                : model::ModelResult<frontend::Snapshot>{oversizedItemSnapshot.error()};
        const bool oversizedItemEnvelope =
            oversizedItemWire && frontend::Codec::encodeServer(frontend::ServerMessage{oversizedItemWire.value()}).hasValue();
        const model::ItemData* oversizedProjectedItem =
            oversizedItemSnapshot && oversizedItemSnapshot.value().items.size() == 1
                ? &model::itemData(oversizedItemSnapshot.value().items.front())
                : nullptr;
        result.expectTrue(
            oversizedProjectedItem && oversizedProjectedItem->agentText == retainedItemContent &&
                oversizedProjectedItem->reasoningText == retainedItemContent &&
                oversizedProjectedItem->reasoningSummary == retainedItemContent &&
                oversizedProjectedItem->commandOutput == retainedItemContent && oversizedProjectedItem->contentTruncated &&
                oversizedProjectedItem->droppedContentBytes == std::optional<std::uint64_t>{16} &&
                oversizedProjectedItem->truncation.truncated && oversizedProjectedItem->truncation.droppedBytes == 16 &&
                oversizedProjectedItem->truncation.omittedPaths ==
                    std::vector<std::string>{"/agentText", "/reasoningText", "/reasoningSummary", "/commandOutput"} &&
                oversizedItemEnvelope,
            "all accumulated item channels retain 16,384 Unicode characters, report the omitted bytes, and remain wire encodable");

        ai::openai::codex::backend::ItemContentChanged oversizedContentEvent;
        oversizedContentEvent.threadId = ai::openai::codex::typed::ThreadId{thread.id};
        oversizedContentEvent.turnId = ai::openai::codex::typed::TurnId{turn.id};
        oversizedContentEvent.itemId = ai::openai::codex::typed::ItemId{item.id};
        oversizedContentEvent.kind = ai::openai::codex::backend::ItemContentChanged::Kind::CommandOutput;
        const std::vector<ai::openai::codex::backend::SequencedBackendEvent> oversizedContentEvents{
            {ai::openai::codex::backend::SequenceNumber{18}, std::move(oversizedContentEvent)}};
        const auto oversizedContentOccurrence = projection.projectOccurrences(oversizedContentEvents, oversizedItemSource);
        const std::vector<ai::openai::codex::backend::ItemSnapshot> oversizedContentItems{oversizedItem};
        const auto directOversizedContentOccurrence =
            projection.projectItemContentOccurrences(oversizedContentEvents, oversizedContentItems);
        const auto* boundedContentOccurrence =
            oversizedContentOccurrence && oversizedContentOccurrence.value().occurrences.size() == 1 &&
                    oversizedContentOccurrence.value().occurrences.front().occurrence.expandedPayloads.size() == 1
                ? std::get_if<model::ItemContentUpdatedOccurrence>(
                      &oversizedContentOccurrence.value().occurrences.front().occurrence.expandedPayloads.front())
                : nullptr;
        result.expectTrue(boundedContentOccurrence && boundedContentOccurrence->content == retainedItemContent &&
                              boundedContentOccurrence->truncation.truncated &&
                              boundedContentOccurrence->truncation.droppedBytes == 16 && directOversizedContentOccurrence &&
                              directOversizedContentOccurrence.value().occurrences == oversizedContentOccurrence.value().occurrences,
                          "exact-item content projection preserves the same bounded replacement occurrence without projecting full State");

        const auto contentEvent = [&](backend::SequenceNumber sequence, backend::ItemContentChanged::Kind kind) {
            backend::ItemContentChanged content;
            content.threadId = typed::ThreadId{thread.id};
            content.turnId = typed::TurnId{turn.id};
            content.itemId = typed::ItemId{item.id};
            content.kind = kind;
            content.delta = "ignored-by-replacement-projection";
            return backend::SequencedBackendEvent{sequence, std::move(content)};
        };
        const std::vector<backend::SequencedBackendEvent> allContentEvents{
            contentEvent(backend::SequenceNumber{19}, backend::ItemContentChanged::Kind::AgentText),
            contentEvent(backend::SequenceNumber{20}, backend::ItemContentChanged::Kind::ReasoningText),
            contentEvent(backend::SequenceNumber{21}, backend::ItemContentChanged::Kind::ReasoningSummary),
            contentEvent(backend::SequenceNumber{22}, backend::ItemContentChanged::Kind::CommandOutput),
        };
        const std::vector<backend::ItemSnapshot> allContentItems(allContentEvents.size(), oversizedItem);
        const auto fullAllContent = projection.projectOccurrences(allContentEvents, oversizedItemSource);
        const auto directAllContent = projection.projectItemContentOccurrences(allContentEvents, allContentItems);
        std::vector<backend::ItemSnapshot> mismatchedContentItems = allContentItems;
        mismatchedContentItems.back().id = "other-item";
        const auto mismatchedContent = projection.projectItemContentOccurrences(allContentEvents, mismatchedContentItems);
        result.expectTrue(fullAllContent && directAllContent && directAllContent.value().occurrences.size() == 4 &&
                              directAllContent.value().occurrences == fullAllContent.value().occurrences && !mismatchedContent,
                          "all content channels use exact replacement semantics and a mismatched batch cannot project a partial prefix");

        const auto hintedContentEvent = [&](backend::SequenceNumber sequence,
                                            std::size_t channelBytesBefore,
                                            std::uint64_t droppedContentBytesBefore) {
            backend::ItemContentChanged content;
            content.threadId = typed::ThreadId{thread.id};
            content.turnId = typed::TurnId{turn.id};
            content.itemId = typed::ItemId{item.id};
            content.kind = backend::ItemContentChanged::Kind::CommandOutput;
            content.delta = "x";
            content.channelBytesBefore = channelBytesBefore;
            content.droppedContentBytesBefore = droppedContentBytesBefore;
            return backend::SequencedBackendEvent{sequence, std::move(content)};
        };
        const std::string asciiPrefix(16'384, 'a');
        backend::ItemSnapshot asciiFirstOverflow = oversizedItem;
        asciiFirstOverflow.agentText.clear();
        asciiFirstOverflow.reasoningText.clear();
        asciiFirstOverflow.reasoningSummary.clear();
        asciiFirstOverflow.commandOutput = asciiPrefix + "1";
        asciiFirstOverflow.droppedContentBytes = 0;
        asciiFirstOverflow.contentTruncated = false;
        backend::ItemSnapshot asciiRedundant = asciiFirstOverflow;
        asciiRedundant.commandOutput += "2";
        backend::ItemSnapshot asciiMissingHints = asciiRedundant;
        asciiMissingHints.commandOutput += "3";
        backend::ItemSnapshot asciiPriorRolling = asciiMissingHints;
        asciiPriorRolling.commandOutput += "4";
        backend::ItemSnapshot asciiCurrentRolling = asciiPriorRolling;
        asciiCurrentRolling.commandOutput += "5";
        asciiCurrentRolling.droppedContentBytes = 1;
        asciiCurrentRolling.contentTruncated = true;

        backend::SequencedBackendEvent missingHintEvent =
            hintedContentEvent(backend::SequenceNumber{25}, asciiPrefix.size() + 2, 0);
        auto* missingHint = std::get_if<backend::ItemContentChanged>(&missingHintEvent.event);
        missingHint->channelBytesBefore.reset();
        missingHint->droppedContentBytesBefore.reset();
        const std::vector<backend::SequencedBackendEvent> hintedEvents{
            hintedContentEvent(backend::SequenceNumber{23}, asciiPrefix.size(), 0),
            hintedContentEvent(backend::SequenceNumber{24}, asciiPrefix.size() + 1, 0),
            std::move(missingHintEvent),
            hintedContentEvent(backend::SequenceNumber{26}, asciiPrefix.size() + 3, 1),
            hintedContentEvent(backend::SequenceNumber{27}, asciiPrefix.size() + 4, 0),
        };
        const std::vector<backend::ItemSnapshot> hintedItems{
            asciiFirstOverflow, asciiRedundant, asciiMissingHints, asciiPriorRolling, asciiCurrentRolling};
        const auto hintedOccurrences = projection.projectItemContentOccurrences(hintedEvents, hintedItems);
        const std::vector<backend::SequencedBackendEvent> redundantOnlyEvents{
            hintedContentEvent(backend::SequenceNumber{24}, asciiPrefix.size() + 1, 0)};
        const std::vector<backend::ItemSnapshot> redundantOnlyItems{asciiRedundant};
        const auto redundantOnly = projection.projectItemContentOccurrences(redundantOnlyEvents, redundantOnlyItems);
        const bool retainedHintedSources =
            hintedOccurrences && hintedOccurrences.value().occurrences.size() == 4 &&
            hintedOccurrences.value().occurrences[0].occurrence.sourceStamp == model::SourceStamp{"backend-event:23"} &&
            hintedOccurrences.value().occurrences[1].occurrence.sourceStamp == model::SourceStamp{"backend-event:25"} &&
            hintedOccurrences.value().occurrences[2].occurrence.sourceStamp == model::SourceStamp{"backend-event:26"} &&
            hintedOccurrences.value().occurrences[3].occurrence.sourceStamp == model::SourceStamp{"backend-event:27"};
        ai::openai::codex::backend::Snapshot conservativeFallbackSource = source;
        conservativeFallbackSource.threads.front().turns.front().items.front() = asciiRedundant;
        const std::vector<backend::SequencedBackendEvent> hintedFallbackEvents{
            hintedContentEvent(backend::SequenceNumber{24}, asciiPrefix.size() + 1, 0)};
        const auto hintedFallback = projection.projectOccurrences(hintedFallbackEvents, conservativeFallbackSource);
        result.expectTrue(retainedHintedSources && redundantOnly && redundantOnly.value().occurrences.empty() &&
                              !redundantOnly.value().snapshotRequired && hintedFallback &&
                              hintedFallback.value().occurrences.size() == 1,
                          "exact-item projection suppresses only provably unchanged post-prefix updates while equality, missing hints, "
                          "backend rolling retention, and the full fallback remain observable");

        const std::string unicodePrefix = std::string(16'383, 'u') + "\xE2\x82\xAC";
        backend::ItemSnapshot unicodeFirstOverflow = asciiFirstOverflow;
        unicodeFirstOverflow.commandOutput = unicodePrefix + "x";
        backend::ItemSnapshot unicodeRedundant = unicodeFirstOverflow;
        unicodeRedundant.commandOutput += "y";
        const std::vector<backend::SequencedBackendEvent> unicodeEvents{
            hintedContentEvent(backend::SequenceNumber{28}, unicodePrefix.size(), 0),
            hintedContentEvent(backend::SequenceNumber{29}, unicodePrefix.size() + 1, 0),
        };
        const std::vector<backend::ItemSnapshot> unicodeItems{unicodeFirstOverflow, unicodeRedundant};
        const auto unicodeOccurrences = projection.projectItemContentOccurrences(unicodeEvents, unicodeItems);
        const auto* unicodeUpdate =
            unicodeOccurrences && unicodeOccurrences.value().occurrences.size() == 1 &&
                    unicodeOccurrences.value().occurrences.front().occurrence.expandedPayloads.size() == 1
                ? std::get_if<model::ItemContentUpdatedOccurrence>(
                      &unicodeOccurrences.value().occurrences.front().occurrence.expandedPayloads.front())
                : nullptr;
        result.expectTrue(unicodeUpdate && unicodeUpdate->content == unicodePrefix && unicodeUpdate->truncation.truncated &&
                              unicodeOccurrences.value().occurrences.front().occurrence.sourceStamp ==
                                  model::SourceStamp{"backend-event:28"},
                          "the strict overflow boundary compares UTF-8 prefix bytes, emits the first multibyte truncation transition, "
                          "and suppresses only its unchanged suffix");

        ai::openai::codex::backend::Snapshot oversizedScalarSource = source;
        const std::string retainedScalar = std::string(16'383, 's') + "\xE2\x82\xAC";
        const std::string oversizedScalar = retainedScalar + "tail";
        const std::string retainedMethod = std::string(1'023, 'm') + "\xE2\x82\xAC";
        const std::string retainedStatus = std::string(255, 'q') + "\xE2\x82\xAC";
        oversizedScalarSource.provider.lastError =
            ai::openai::codex::backend::ErrorSnapshot{"provider", 7, oversizedScalar};
        oversizedScalarSource.threadList.nextCursor = oversizedScalar;
        oversizedScalarSource.threads.front().title = oversizedScalar;
        oversizedScalarSource.threads.front().realtime.transcript = oversizedScalar;
        oversizedScalarSource.models.latestResults.push_back({retainedMethod + "tail",
                                                               0,
                                                               retainedStatus + "tail",
                                                               retainedMethod + "tail",
                                                               oversizedScalar,
                                                               1,
                                                               true,
                                                               {}});
        const auto oversizedScalarSnapshot = projection.projectSnapshot(oversizedScalarSource);
        const auto oversizedScalarWire =
            oversizedScalarSnapshot
                ? model::encodeProjectedSnapshot(oversizedScalarSnapshot.value(),
                                                  model::SnapshotRepresentationSelection{true, true, true, true})
                : model::ModelResult<frontend::Snapshot>{oversizedScalarSnapshot.error()};
        const bool oversizedScalarEnvelope =
            oversizedScalarWire && frontend::Codec::encodeServer(frontend::ServerMessage{oversizedScalarWire.value()}).hasValue();
        const frontend::Json oversizedThreadDetails =
            oversizedScalarSnapshot && oversizedScalarSnapshot.value().threads.size() == 1
                ? oversizedScalarSnapshot.value().threads.front().safeDetails.json()
                : frontend::Json::object();
        const model::DomainState* oversizedModels =
            oversizedScalarSnapshot ? &oversizedScalarSnapshot.value().models.state : nullptr;
        result.expectTrue(
            oversizedScalarEnvelope && oversizedScalarSnapshot.value().threads.front().title == retainedScalar &&
                oversizedScalarSnapshot.value().threadList.nextCursor == retainedScalar &&
                oversizedThreadDetails.at("realtime").value("transcript", std::string{}) == retainedScalar && oversizedModels &&
                oversizedModels->status == retainedStatus && oversizedModels->nextCursor == retainedScalar &&
                oversizedModels->latestResults.size() == 1 && oversizedModels->latestResults.front().method == retainedMethod &&
                oversizedModels->latestResults.front().subjectId == std::optional<std::string>{retainedMethod} &&
                oversizedModels->latestResults.front().nextCursor == std::optional<std::string>{retainedScalar} &&
                oversizedScalarSnapshot.value().truncation.truncated,
            "provider, thread, cursor, realtime, and domain strings share the same Unicode-safe frontend bounds");

        ai::openai::codex::backend::Snapshot nestedPendingSource = source;
        nestedPendingSource.pendingRequests.front().details =
            frontend::Json{{"summary", "approval"}, {"reason", oversizedScalar}, {"params", {{"nested", "omitted"}}}};
        const auto nestedPendingSnapshot = projection.projectSnapshot(nestedPendingSource);
        const auto nestedPendingWire =
            nestedPendingSnapshot
                ? model::encodeProjectedSnapshot(nestedPendingSnapshot.value(),
                                                  model::SnapshotRepresentationSelection{true, true, true, true})
                : model::ModelResult<frontend::Snapshot>{nestedPendingSnapshot.error()};
        const model::PendingRequestData* nestedPending =
            nestedPendingSnapshot && nestedPendingSnapshot.value().pendingRequests.size() == 1
                ? &model::pendingRequestData(nestedPendingSnapshot.value().pendingRequests.front())
                : nullptr;
        result.expectTrue(nestedPendingWire &&
                              frontend::Codec::encodeServer(frontend::ServerMessage{nestedPendingWire.value()}).hasValue() &&
                              nestedPending && nestedPending->safeDetails &&
                              nestedPending->safeDetails->json().value("reason", std::string{}) == retainedScalar &&
                              !nestedPending->safeDetails->json().contains("params") && nestedPending->truncation.truncated,
                          "pending-request details omit nested provider data and bound retained scalar presentation fields");

        ai::openai::codex::backend::Snapshot excessSessionsSource = source;
        excessSessionsSource.sessions.clear();
        for (std::uint64_t id = 1; id <= 129; ++id) {
            excessSessionsSource.sessions.push_back(
                {ai::openai::codex::backend::SessionId{id}, ai::openai::codex::backend::SessionRole::Observer});
        }
        const auto excessSessionsSnapshot = projection.projectSnapshot(excessSessionsSource);
        const auto excessSessionsWire =
            excessSessionsSnapshot
                ? model::encodeProjectedSnapshot(excessSessionsSnapshot.value(),
                                                  model::SnapshotRepresentationSelection{true, true, true, true})
                : model::ModelResult<frontend::Snapshot>{excessSessionsSnapshot.error()};
        result.expectTrue(excessSessionsWire && excessSessionsSnapshot.value().sessions.size() == 128 &&
                              excessSessionsSnapshot.value().truncation.truncated &&
                              frontend::Codec::encodeServer(frontend::ServerMessage{excessSessionsWire.value()}).hasValue(),
                          "custom backend capacities cannot project more entries than the frozen frontend collection bound");

        ai::openai::codex::backend::Snapshot itemDetailsSource = source;
        ai::openai::codex::backend::ItemSnapshot& userItem = itemDetailsSource.threads.front().turns.front().items.front();
        userItem.type = "user_message";
        const frontend::Json sourceUserContent = frontend::Json::array(
            {frontend::Json{{"type", "text"}, {"text", "hello"}},
             frontend::Json{{"type", "image"}, {"url", "https://private.invalid/MUST_NOT_REACH_FRONTEND_IMAGE"}},
             frontend::Json{{"type", "text"}, {"text", "Grüße 🌍"}},
             frontend::Json{{"type", "skill"}, {"name", "private-skill"}, {"path", "/MUST_NOT_REACH_FRONTEND_SKILL"}}});
        userItem.data = frontend::Json{{"clientId", nullptr},
                                       {"content", sourceUserContent},
                                       {"contentTruncated", false},
                                       {"originalContentBytes", sourceUserContent.dump().size()},
                                       {"retainedContentBytes", sourceUserContent.dump().size()},
                                       {"originalContentItems", 4},
                                       {"retainedContentItems", 4}};
        userItem.extensions = frontend::Json{{"accessToken", "MUST_NOT_REACH_FRONTEND"}};
        const auto itemDetailsSnapshot = projection.projectSnapshot(itemDetailsSource);
        const auto itemDetailsWire = itemDetailsSnapshot
                                         ? model::encodeProjectedSnapshot(itemDetailsSnapshot.value(),
                                                                          model::SnapshotRepresentationSelection{true, true, true, true})
                                         : model::ModelResult<frontend::Snapshot>{itemDetailsSnapshot.error()};
        const frontend::Json projectedItemDetails = itemDetailsSnapshot && itemDetailsSnapshot.value().items.size() == 1 &&
                                                            model::itemData(itemDetailsSnapshot.value().items.front()).safeDetails
                                                        ? model::itemData(itemDetailsSnapshot.value().items.front()).safeDetails->json()
                                                        : frontend::Json::object();
        const frontend::Json wireItemDetails =
            itemDetailsWire && itemDetailsWire.value().state.contains("items") && !itemDetailsWire.value().state.at("items").empty()
                ? itemDetailsWire.value().state.at("items").front().value("data", frontend::Json::object())
                : frontend::Json::object();
        result.expectTrue(itemDetailsWire && projectedItemDetails.contains("content") && projectedItemDetails.at("content").is_array() &&
                              projectedItemDetails.at("content").size() == 4 && projectedItemDetails.at("content").front().is_object() &&
                              !wireItemDetails.contains("content") && wireItemDetails.contains("clientId") &&
                              wireItemDetails.at("clientId").is_null() && wireItemDetails.value("contentTruncated", true) == false &&
                              wireItemDetails.value("text", std::string{}) == "hello\n\nGrüße 🌍" &&
                              !wireItemDetails.value("textTruncated", true) && wireItemDetails.value("originalContentItems", 0) == 4 &&
                              wireItemDetails.value("retainedContentItems", 0) == 4 && projectedItemDetails.contains("clientId") &&
                              projectedItemDetails.at("clientId").is_null() && projectedItemDetails.value("originalContentItems", 0) == 4 &&
                              itemDetailsWire.value().state.dump().find("MUST_NOT_REACH_FRONTEND") == std::string::npos &&
                              itemDetailsWire.value().state.dump().find("private-skill") == std::string::npos,
                          "canonical item details retain bounded provider content while the expanded-wire seam emits only "
                          "ordered UTF-8 user text and no non-text or sensitive values");

        ai::openai::codex::backend::Snapshot longTextSource = source;
        ai::openai::codex::backend::ItemSnapshot& longTextItem = longTextSource.threads.front().turns.front().items.front();
        longTextItem.type = "user_message";
        const std::string sourceUserText = std::string(16'383, 'u') + "€" + std::string(16'381, 'v');
        const frontend::Json longTextContent =
            frontend::Json::array({frontend::Json{{"type", "text"}, {"text", sourceUserText}}});
        longTextItem.data = frontend::Json{{"clientId", "client-long"},
                                           {"content", longTextContent},
                                           {"contentTruncated", false},
                                           {"originalContentBytes", longTextContent.dump().size()},
                                           {"retainedContentBytes", longTextContent.dump().size()},
                                           {"originalContentItems", 1},
                                           {"retainedContentItems", 1}};
        const auto longTextSnapshot = projection.projectSnapshot(longTextSource);
        const auto longTextWire = longTextSnapshot
                                      ? model::encodeProjectedSnapshot(longTextSnapshot.value(),
                                                                       model::SnapshotRepresentationSelection{true, true, true, true})
                                      : model::ModelResult<frontend::Snapshot>{longTextSnapshot.error()};
        const frontend::Json projectedLongText = longTextSnapshot && longTextSnapshot.value().items.size() == 1 &&
                                                         model::itemData(longTextSnapshot.value().items.front()).safeDetails
                                                     ? model::itemData(longTextSnapshot.value().items.front()).safeDetails->json()
                                                     : frontend::Json::object();
        const frontend::Json wireLongText =
            longTextWire && longTextWire.value().state.contains("items") && !longTextWire.value().state.at("items").empty()
                ? longTextWire.value().state.at("items").front().value("data", frontend::Json::object())
                : frontend::Json::object();
        result.expectTrue(longTextWire && projectedLongText.at("content") == longTextContent &&
                              !projectedLongText.value("contentTruncated", true) && !wireLongText.contains("content") &&
                              wireLongText.value("text", std::string{}) == std::string(16'383, 'u') + "\xE2\x82\xAC" &&
                              wireLongText.value("textTruncated", false) && !wireLongText.value("contentTruncated", true) &&
                              wireLongText.value("originalContentBytes", 0U) == longTextContent.dump().size() &&
                              wireLongText.value("retainedContentBytes", 0U) == longTextContent.dump().size(),
                          "a complete 32 KiB structured message remains intact while rendered text is independently truncated at "
                          "16,384 Unicode characters");

        ai::openai::codex::backend::Snapshot truncatedContentSource = source;
        ai::openai::codex::backend::ItemSnapshot& truncatedContentItem =
            truncatedContentSource.threads.front().turns.front().items.front();
        truncatedContentItem.type = "user_message";
        const frontend::Json retainedPrefix =
            frontend::Json::array({frontend::Json{{"type", "text"}, {"text", "retained text"}}});
        truncatedContentItem.data = frontend::Json{{"clientId", nullptr},
                                                   {"content", retainedPrefix},
                                                   {"contentTruncated", true},
                                                   {"originalContentBytes", retainedPrefix.dump().size() + 100},
                                                   {"retainedContentBytes", retainedPrefix.dump().size()},
                                                   {"originalContentItems", 2},
                                                   {"retainedContentItems", 1}};
        const auto truncatedContentSnapshot = projection.projectSnapshot(truncatedContentSource);
        const auto truncatedContentWire =
            truncatedContentSnapshot
                ? model::encodeProjectedSnapshot(truncatedContentSnapshot.value(),
                                                  model::SnapshotRepresentationSelection{true, true, true, true})
                : model::ModelResult<frontend::Snapshot>{truncatedContentSnapshot.error()};
        const frontend::Json wireTruncatedContent =
            truncatedContentWire && truncatedContentWire.value().state.contains("items") &&
                    !truncatedContentWire.value().state.at("items").empty()
                ? truncatedContentWire.value().state.at("items").front().value("data", frontend::Json::object())
                : frontend::Json::object();
        result.expectTrue(truncatedContentWire && wireTruncatedContent.value("text", std::string{}) == "retained text" &&
                              !wireTruncatedContent.value("textTruncated", true) &&
                              wireTruncatedContent.value("contentTruncated", false),
                          "omitted backend content remains distinct from complete rendering of the retained textual prefix");

        ai::openai::codex::backend::ItemContentChanged content;
        content.threadId = ai::openai::codex::typed::ThreadId{thread.id};
        content.turnId = ai::openai::codex::typed::TurnId{turn.id};
        content.itemId = ai::openai::codex::typed::ItemId{item.id};
        content.kind = ai::openai::codex::backend::ItemContentChanged::Kind::AgentText;
        content.delta = "ignored delta";
        ai::openai::codex::backend::CodexExtensionReceived warning;
        warning.method = "configWarning";
        warning.payload =
            frontend::Json{{"summary", "event-derived configuration warning"}, {"details", "event-derived configuration detail"}};
        warning.safeProjection = true;
        ai::openai::codex::backend::CodexExtensionReceived secondWarning;
        secondWarning.method = "warning";
        secondWarning.payload = frontend::Json{{"message", "second event-derived warning"}, {"threadId", thread.id}};
        secondWarning.safeProjection = true;
        ai::openai::codex::backend::CodexExtensionReceived unknown;
        unknown.method = "future/notification";
        unknown.payload = frontend::Json{{"accessToken", "must-not-enter-the-model"}};
        unknown.safeProjection = true;
        std::vector<ai::openai::codex::backend::SequencedBackendEvent> events;
        events.push_back({ai::openai::codex::backend::SequenceNumber{18}, std::move(content)});
        events.push_back({ai::openai::codex::backend::SequenceNumber{19}, std::move(warning)});
        events.push_back({ai::openai::codex::backend::SequenceNumber{20}, std::move(secondWarning)});
        events.push_back({ai::openai::codex::backend::SequenceNumber{21}, std::move(unknown)});
        const auto occurrences = projection.projectOccurrences(events, source);
        const bool contentFromSnapshot =
            occurrences && occurrences.value().occurrences.size() == 4 &&
            occurrences.value().occurrences[0].occurrence.expandedPayloads.size() == 1 &&
            std::get<model::ItemContentUpdatedOccurrence>(occurrences.value().occurrences[0].occurrence.expandedPayloads.front()).content ==
                "canonical accumulated content";
        const bool generatedGroup = occurrences &&
                                    occurrences.value().occurrences[1].occurrence.expandedPayloads.size() == 2 &&
                                    model::occurrenceType(
                                        occurrences.value().occurrences[1].occurrence.expandedPayloads[0]) ==
                                        frontend::ExpandedEventType::ConfigurationUpdated &&
                                    model::occurrenceType(
                                        occurrences.value().occurrences[1].occurrence.expandedPayloads[1]) ==
                                        frontend::ExpandedEventType::NoticeAdded;
        const auto* firstNotice =
            generatedGroup ? &std::get<model::NoticeAddedOccurrence>(occurrences.value().occurrences[1].occurrence.expandedPayloads[1])
                           : nullptr;
        const auto* secondNotice =
            occurrences && occurrences.value().occurrences[2].occurrence.expandedPayloads.size() == 1
                ? std::get_if<model::NoticeAddedOccurrence>(&occurrences.value().occurrences[2].occurrence.expandedPayloads.front())
                : nullptr;
        const bool exactNoticeEvents = firstNotice != nullptr && secondNotice != nullptr &&
                                       firstNotice->notice.summary == "event-derived configuration warning" &&
                                       firstNotice->notice.details == std::optional<std::string>{"event-derived configuration detail"} &&
                                       secondNotice->notice.summary == "second event-derived warning" &&
                                       secondNotice->notice.threadId == model::ThreadIdentity::parse(thread.id);
        const bool containedUnknown =
            occurrences && occurrences.value().occurrences[3].occurrence.expandedPayloads.empty() &&
            occurrences.value().occurrences[3].occurrence.legacyCompatibility.safeExtension &&
            occurrences.value().occurrences[3].occurrence.legacyCompatibility.safeExtension->sensitiveFieldsRedacted &&
            occurrences.value().occurrences[3].occurrence.legacyCompatibility.safeExtension->params.empty();

        ai::openai::codex::backend::Snapshot withoutRetainedNotices = source;
        withoutRetainedNotices.notices.clear();
        ai::openai::codex::backend::CodexExtensionReceived retainedIndependent;
        retainedIndependent.method = "configWarning";
        retainedIndependent.payload = frontend::Json{{"summary", "retention-independent warning"}};
        retainedIndependent.safeProjection = true;
        const std::vector<ai::openai::codex::backend::SequencedBackendEvent> retentionEvents{
            {ai::openai::codex::backend::SequenceNumber{22}, std::move(retainedIndependent)}};
        const auto retentionIndependentProjection = projection.projectOccurrences(retentionEvents, withoutRetainedNotices);
        const auto* retentionNotice =
            retentionIndependentProjection && retentionIndependentProjection.value().occurrences.size() == 1 &&
                    retentionIndependentProjection.value().occurrences.front().occurrence.expandedPayloads.size() == 2
                ? std::get_if<model::NoticeAddedOccurrence>(
                      &retentionIndependentProjection.value().occurrences.front().occurrence.expandedPayloads[1])
                : nullptr;

        ai::openai::codex::backend::CodexExtensionReceived malformedWarning;
        malformedWarning.method = "configWarning";
        malformedWarning.payload = frontend::Json{{"details", "missing required summary"}};
        malformedWarning.safeProjection = true;
        const std::vector<ai::openai::codex::backend::SequencedBackendEvent> malformedEvents{
            {ai::openai::codex::backend::SequenceNumber{23}, std::move(malformedWarning)}};
        const auto malformedProjection = projection.projectOccurrences(malformedEvents, source);
        const bool malformedContained =
            malformedProjection && malformedProjection.value().snapshotRequired && malformedProjection.value().occurrences.empty();
        const bool activityRemainsProjectionOnly =
            std::none_of(frontend::generated::AllNotificationProjections.begin(),
                         frontend::generated::AllNotificationProjections.end(),
                         [](const frontend::generated::ProjectionMetadata& metadata) {
                             return std::find(metadata.expandedMappings.begin(),
                                              metadata.expandedMappings.end(),
                                              std::string_view{"activity.updated"}) != metadata.expandedMappings.end();
                         });

        ai::openai::codex::backend::CodexExtensionReceived unsafeConfiguration;
        unsafeConfiguration.method = "configWarning";
        unsafeConfiguration.payload =
            frontend::Json{{"summary", "unsafe configuration summary"}, {"details", "private configuration detail"}};
        ai::openai::codex::backend::CodexExtensionReceived unsafeGuardian;
        unsafeGuardian.method = "guardianWarning";
        unsafeGuardian.payload = frontend::Json{{"message", "private guardian message"}, {"threadId", "private-thread"}};
        ai::openai::codex::backend::CodexExtensionReceived deprecation;
        deprecation.method = "deprecationNotice";
        deprecation.payload = frontend::Json{{"summary", "deprecated behavior"}, {"details", ""}};
        deprecation.safeProjection = true;
        ai::openai::codex::backend::CodexExtensionReceived worldWritable;
        worldWritable.method = "windows/worldWritableWarning";
        worldWritable.payload =
            frontend::Json{{"failedScan", false}, {"samplePaths", frontend::Json::array({"one", "two"})}, {"extraCount", 3}};
        worldWritable.safeProjection = true;
        ai::openai::codex::backend::CodexExtensionReceived utf8Boundary;
        utf8Boundary.method = "warning";
        utf8Boundary.payload = frontend::Json{{"message", std::string(16'383, 'a') + "\xE2\x82\xAC"}};
        utf8Boundary.safeProjection = true;
        ai::openai::codex::backend::CodexExtensionReceived oversizedIdentity;
        oversizedIdentity.method = "warning";
        oversizedIdentity.payload = frontend::Json{{"message", "oversized identity warning"},
                                                   {"threadId", std::string(model::ThreadIdentity::MaximumBytes + 1, 'x')}};
        oversizedIdentity.safeProjection = true;
        const std::vector<ai::openai::codex::backend::SequencedBackendEvent> noticeSecurityEvents{
            {ai::openai::codex::backend::SequenceNumber{24}, std::move(unsafeConfiguration)},
            {ai::openai::codex::backend::SequenceNumber{25}, std::move(unsafeGuardian)},
            {ai::openai::codex::backend::SequenceNumber{26}, std::move(deprecation)},
            {ai::openai::codex::backend::SequenceNumber{27}, std::move(worldWritable)},
            {ai::openai::codex::backend::SequenceNumber{28}, std::move(utf8Boundary)},
            {ai::openai::codex::backend::SequenceNumber{29}, std::move(oversizedIdentity)},
        };
        const auto noticeSecurityProjection = projection.projectOccurrences(noticeSecurityEvents, source);
        const auto noticeAt = [&](std::size_t index) -> const model::NoticeRecord* {
            if (!noticeSecurityProjection || index >= noticeSecurityProjection.value().occurrences.size()) {
                return nullptr;
            }
            const auto& payloads = noticeSecurityProjection.value().occurrences[index].occurrence.expandedPayloads;
            const auto found = std::find_if(payloads.begin(), payloads.end(), [](const model::OccurrencePayload& payload) {
                return model::occurrenceType(payload) == frontend::ExpandedEventType::NoticeAdded;
            });
            const auto* notice = found != payloads.end() ? std::get_if<model::NoticeAddedOccurrence>(&*found) : nullptr;
            return notice != nullptr ? &notice->notice : nullptr;
        };
        const model::NoticeRecord* unsafeConfigurationNotice = noticeAt(0);
        const model::NoticeRecord* unsafeGuardianNotice = noticeAt(1);
        const model::NoticeRecord* deprecationNotice = noticeAt(2);
        const model::NoticeRecord* worldWritableNotice = noticeAt(3);
        const model::NoticeRecord* utf8BoundaryNotice = noticeAt(4);
        const model::NoticeRecord* oversizedIdentityNotice = noticeAt(5);
        const auto* sanitizedCompatibility = noticeSecurityProjection && !noticeSecurityProjection.value().occurrences.empty()
                                                 ? &noticeSecurityProjection.value().occurrences.front().occurrence.legacyCompatibility
                                                 : nullptr;
        const bool sanitizedLegacy =
            sanitizedCompatibility && sanitizedCompatibility->safeExtension &&
            sanitizedCompatibility->safeExtension->sensitiveFieldsRedacted &&
            sanitizedCompatibility->safeExtension->params.json().dump().find("private configuration detail") == std::string::npos;
        result.expectTrue(noticeSecurityProjection && !noticeSecurityProjection.value().snapshotRequired &&
                              noticeSecurityProjection.value().occurrences.size() == 6 && unsafeConfigurationNotice &&
                              unsafeConfigurationNotice->summary == "unsafe configuration summary" &&
                              unsafeConfigurationNotice->details == std::optional<std::string>{"[redacted]"} && unsafeGuardianNotice &&
                              unsafeGuardianNotice->summary == "[redacted]" &&
                              unsafeGuardianNotice->threadId == model::ThreadIdentity::parse("[redacted]") && deprecationNotice &&
                              !deprecationNotice->details && worldWritableNotice &&
                              worldWritableNotice->category == "windows_world_writable" &&
                              worldWritableNotice->details == std::optional<std::string>{"sample paths: 2, additional paths: 3"} &&
                              utf8BoundaryNotice &&
                              utf8BoundaryNotice->summary == std::string(16'383, 'a') + "\xE2\x82\xAC" && oversizedIdentityNotice &&
                              !oversizedIdentityNotice->threadId && sanitizedLegacy,
                          "notice projection sanitizes unsafe extensions, truncates on UTF-8 boundaries, and preserves omission semantics");
        result.expectTrue(
            contentFromSnapshot && generatedGroup && exactNoticeEvents && containedUnknown && !occurrences.value().snapshotRequired &&
                retentionNotice != nullptr && retentionNotice->notice.summary == "retention-independent warning" &&
                !retentionIndependentProjection.value().snapshotRequired && malformedContained && activityRemainsProjectionOnly,
            "live backend events select canonical state, exact event-derived notices, generated mappings, and bounded containment");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testSynchronization(result);
    testTypedBatching(result);
    testScopeFilteredSparseReplay(result);
    testInlineObserverBatchCoalescing(result);
    testTerminalItemKeepsInitialUpsertOrder(result);
    testIndependentSnapshotCapabilities(result);
    testLegacyOnlyPerConnectionContainment(result);
    testSessionAndControllerJournal(result);
    testExactBackendOccurrenceIdentity(result);
    testUnknownBackendItemContainment(result);
    testUnknownBackendItemWireContainment(result);
    testUnknownPendingRequestCompatibility(result);
    testPendingUserInputPresentationUtf8Containment(result);
    testMinimalSnapshotKeepsUserInputActionable(result);
    testBackendProjection(result);
    return result.processResult();
}
