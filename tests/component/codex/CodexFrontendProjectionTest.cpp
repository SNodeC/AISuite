/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/Protocol.h"
#include "ai/openai/codex/frontend/detail/BackendProjectionBuilder.h"
#include "ai/openai/codex/frontend/detail/FrontendProjection.h"
#include "ai/openai/codex/frontend/detail/GeneratedSchemaValidator.h"
#include "support/TestResult.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    namespace detail = ai::openai::codex::frontend::detail;
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = ai::openai::codex::frontend::generated;

    constexpr std::string_view SecretSentinel = "STRUCTURED_SECRET_SENTINEL";
    constexpr std::string_view ArbitraryTextSentinel = "ARBITRARY_TOKEN_SHAPED_TEXT_SENTINEL";

    frontend::FrontendPrincipal principal(std::string id, std::span<const frontend::FrontendScope> scopes, std::string profile) {
        return {std::move(id), std::vector<frontend::FrontendScope>{scopes.begin(), scopes.end()}, std::move(profile), false};
    }

    detail::FrontendProjectionContext localExpandedContext(bool explicitProjectionMetadata = true) {
        const frontend::FrontendPrincipal local =
            principal("local", frontend::LocalTrustedScopes, std::string(frontend::LocalTrustedScopeProfile.name));
        std::vector capabilities{
            frontend::FrontendCapability::CompleteBackendDomains,
            frontend::FrontendCapability::DedicatedPendingRequests,
            frontend::FrontendCapability::DedicatedNotificationEvents,
            frontend::FrontendCapability::CompleteThreadItems,
        };
        if (explicitProjectionMetadata) {
            capabilities.push_back(frontend::FrontendCapability::ScopeProjectedState);
        }
        return detail::makeProjectionContext(local, capabilities);
    }

    detail::FrontendProjectionContext defaultExpandedContext(bool explicitProjectionMetadata = true) {
        const frontend::FrontendPrincipal remote =
            principal("remote", frontend::DefaultRemoteScopes, std::string(frontend::DefaultRemoteScopeProfile.name));
        std::vector capabilities{
            frontend::FrontendCapability::CompleteBackendDomains,
            frontend::FrontendCapability::DedicatedPendingRequests,
            frontend::FrontendCapability::DedicatedNotificationEvents,
            frontend::FrontendCapability::CompleteThreadItems,
        };
        if (explicitProjectionMetadata) {
            capabilities.push_back(frontend::FrontendCapability::ScopeProjectedState);
        }
        return detail::makeProjectionContext(remote, capabilities);
    }

    detail::FrontendProjectionContext localContext(std::initializer_list<frontend::FrontendCapability> capabilities) {
        const frontend::FrontendPrincipal local =
            principal("local", frontend::LocalTrustedScopes, std::string(frontend::LocalTrustedScopeProfile.name));
        return detail::makeProjectionContext(local,
                                             std::span<const frontend::FrontendCapability>{capabilities.begin(), capabilities.size()});
    }

    detail::FrontendProjectionContext defaultContext(std::initializer_list<frontend::FrontendCapability> capabilities) {
        const frontend::FrontendPrincipal remote =
            principal("remote", frontend::DefaultRemoteScopes, std::string(frontend::DefaultRemoteScopeProfile.name));
        return detail::makeProjectionContext(remote,
                                             std::span<const frontend::FrontendCapability>{capabilities.begin(), capabilities.size()});
    }

    detail::FrontendProjectionContext scopedContext(std::initializer_list<frontend::FrontendScope> scopes, bool expanded) {
        const frontend::FrontendPrincipal restricted =
            principal("restricted", std::span<const frontend::FrontendScope>{scopes.begin(), scopes.size()}, "restricted_projection_test");
        std::vector<frontend::FrontendCapability> capabilities{frontend::FrontendCapability::ScopeProjectedState};
        if (expanded) {
            capabilities.push_back(frontend::FrontendCapability::CompleteBackendDomains);
            capabilities.push_back(frontend::FrontendCapability::CompleteThreadItems);
        }
        return detail::makeProjectionContext(restricted, capabilities);
    }

    frontend::Json expandedState() {
        return {
            {"provider", {{"lifecycle", "ready"}, {"generation", 9}}},
            {"controller", {{"present", true}, {"controllerSessionId", "1"}}},
            {"sessions", frontend::Json::array({frontend::Json{{"sessionId", "1"}, {"role", "controller"}}})},
            {"threadList",
             {{"hasLoadedPage", true},
              {"complete", false},
              {"pagesLoaded", 3},
              {"nextCursor", "next-thread-page"},
              {"backwardsCursor", "previous-thread-page"},
              {"stamp", {{"generation", 9}, {"freshness", "current"}}}}},
            {"threads", frontend::Json::array()},
            {"turns", frontend::Json::array()},
            {"items",
             frontend::Json::array({frontend::Json{{"id", "command-item"},
                                                   {"type", "commandExecution"},
                                                   {"commandOutput", "bounded privileged output"},
                                                   {"location", {{"path", "/private/worktree/file.cpp"}}}}})},
            {"pendingRequests",
             frontend::Json::array({frontend::Json{{"pendingRequestId", "8"},
                                                   {"kind", "command_execution_approval"},
                                                   {"details", {{"summary", "safe approval summary"}, {"accessToken", SecretSentinel}}}}})},
            {"processes",
             {{"entries",
               frontend::Json::array({frontend::Json{{"processHandle", "process-1"},
                                                     {"lifecycle", "running"},
                                                     {"command", "printf safe"},
                                                     {"output", "bounded privileged output"},
                                                     {"stamp", {{"generation", 9}, {"freshness", "current"}}}}})}}},
            {"filesystemWatches",
             {{"entries",
               frontend::Json::array({frontend::Json{
                   {"path", "/private/worktree"}, {"watchId", "watch-1"}, {"stamp", {{"generation", 9}, {"freshness", "current"}}}}})}}},
            {"publicNote", "visible"},
            {"accessToken", SecretSentinel},
            {"rawProviderEnvelope", {{"message", SecretSentinel}}},
            {"capacity", frontend::Json::object()},
            {"truncation", {{"truncated", false}, {"omittedFields", frontend::Json::array()}}},
        };
    }

    detail::CanonicalSnapshotRecord canonicalSnapshot() {
        frontend::Json legacy = expandedState();
        legacy["legacy"] = true;
        detail::CanonicalSnapshotRecord input;
        input.sequence = frontend::SequenceNumber{41};
        input.legacyState = {legacy,
                             {{"/publicNote", {frontend::FrontendScope::AccountManagement}, detail::ScopeProjectionAction::Redact}}};
        input.expandedState = {
            expandedState(),
            {{"/publicNote", {frontend::FrontendScope::AccountManagement}, detail::ScopeProjectionAction::Redact},
             {"/items/*/commandOutput", {frontend::FrontendScope::CommandExecution}, detail::ScopeProjectionAction::Omit},
             {"/processes", {frontend::FrontendScope::CommandExecution}, detail::ScopeProjectionAction::Omit},
             {"/filesystemWatches", {frontend::FrontendScope::FilesystemRead}, detail::ScopeProjectionAction::Omit}}};
        input.extensions = {{"safeExtension", true}, {"credential", SecretSentinel}};
        return detail::canonicalizeSnapshot(std::move(input));
    }

    detail::CanonicalEventRecord canonicalEvent() {
        detail::CanonicalEventRecord input;
        input.sequence = frontend::SequenceNumber{42};
        input.legacyType = "codex.extension";
        input.legacyData = {{{"method", "process/outputDelta"},
                             {"delta", "bounded privileged output"},
                             {"accessToken", SecretSentinel},
                             {"rawProviderEnvelope", {{"message", SecretSentinel}}}},
                            {{"/delta", {frontend::FrontendScope::CommandExecution}, detail::ScopeProjectionAction::Omit}}};
        input.expandedEvents = {
            {frontend::ExpandedEventType::ProcessUpdated,
             {{{"process",
                {{"processHandle", "process-1"},
                 {"lifecycle", "running"},
                 {"command", "printf safe"},
                 {"stdout", "bounded privileged output"},
                 {"stamp", {{"generation", 9}, {"freshness", "current"}}},
                 {"password", SecretSentinel}}}},
              {{"/process/command", {frontend::FrontendScope::CommandExecution}, detail::ScopeProjectionAction::Omit},
               {"/process/stdout", {frontend::FrontendScope::CommandExecution}, detail::ScopeProjectionAction::Omit}}},
             {}},
            {frontend::ExpandedEventType::NoticeAdded,
             {{{"notice", {{"category", "review"}, {"summary", "safe notice"}, {"stamp", {{"generation", 9}, {"freshness", "current"}}}}}},
              {}},
             {}},
        };
        input.registryKey = "server_notification:ServerNotification:method:process/outputDelta";
        input.extensions = {{"safeExtension", true}, {"apiKey", SecretSentinel}};
        return detail::canonicalizeEvent(std::move(input));
    }

    bool serializedContainsSecret(const frontend::Json& value) {
        return value.dump().find(SecretSentinel) != std::string::npos;
    }

    ai::openai::codex::backend::Snapshot representativeBackendSnapshot() {
        namespace backend = ai::openai::codex::backend;
        backend::Snapshot snapshot;
        snapshot.provider.lifecycle = backend::ProviderLifecycle::Ready;
        snapshot.provider.generation = 9;
        snapshot.provider.desiredRunning = true;
        snapshot.provider.recovery.status = backend::RecoveryStatus::Idle;
        snapshot.controller = backend::SessionId{1};
        snapshot.sessions.push_back({backend::SessionId{1}, backend::SessionRole::Controller});
        snapshot.threadList.hasLoadedPage = true;
        snapshot.threadList.complete = false;
        snapshot.threadList.pagesLoaded = 3;
        snapshot.threadList.nextCursor = "next-thread-page";
        snapshot.threadList.backwardsCursor = "previous-thread-page";
        snapshot.threadList.stamp = {9, backend::Freshness::Current};

        backend::ItemSnapshot item;
        item.id = "item-1";
        item.type = "commandExecution";
        item.status = "completed";
        item.commandOutput = "bounded privileged output";
        item.data = {{"command", "PRIVILEGED_COMMAND"},
                     {"cwd", "/PRIVILEGED_CWD"},
                     {"changes", frontend::Json::array({{{"path", "/PRIVILEGED_PATH"}, {"diff", "PRIVILEGED_DIFF"}}})},
                     {"accessToken", SecretSentinel}};
        item.extensions = {{"privatePresentation", "PRIVILEGED_ITEM_EXTENSION"}};
        item.stamp = {9, backend::Freshness::Current};
        backend::TurnSnapshot turn;
        turn.id = "turn-1";
        turn.threadId = "thread-1";
        turn.status = "completed";
        turn.terminal = true;
        turn.extensions = {{"privatePresentation", "PRIVILEGED_TURN_EXTENSION"}};
        turn.stamp = {9, backend::Freshness::Current};
        turn.items.push_back(std::move(item));
        backend::ThreadSnapshot thread;
        thread.id = "thread-1";
        thread.title = "Projection thread";
        thread.cwd = "/PRIVILEGED_THREAD_CWD";
        thread.fullyLoaded = true;
        thread.extensions = {{"privatePresentation", "PRIVILEGED_THREAD_EXTENSION"}};
        thread.stamp = {9, backend::Freshness::Current};
        thread.turns.push_back(std::move(turn));
        snapshot.threads.push_back(std::move(thread));

        backend::PendingRequestSnapshot pending;
        pending.id = backend::PendingRequestId{8};
        pending.type = "command_approval";
        pending.threadId = "thread-1";
        pending.turnId = "turn-1";
        pending.itemId = "item-1";
        pending.details = {{"summary", "safe approval summary"}, {"credential", SecretSentinel}};
        snapshot.pendingRequests.push_back(std::move(pending));

        backend::ProcessSnapshot process;
        process.processHandle = "process-1";
        process.lifecycle = "running";
        process.stdoutBytes = 24;
        process.stamp = {9, backend::Freshness::Current};
        snapshot.processes.push_back(std::move(process));
        backend::FilesystemWatchSnapshot watch;
        watch.watchId = "watch-1";
        watch.root = "/private/worktree";
        watch.stamp = {9, backend::Freshness::Current};
        snapshot.filesystemWatches.push_back(std::move(watch));
        backend::FuzzySearchSnapshot search;
        search.sessionId = "search-1";
        search.resultCount = 2;
        search.complete = false;
        search.stamp = {9, backend::Freshness::Current};
        snapshot.fuzzySearchSessions.push_back(std::move(search));
        backend::NoticeSnapshot notice;
        notice.occurrence = 1;
        notice.category = backend::NoticeCategory::Warning;
        notice.summary = "representative notice";
        notice.stamp = {9, backend::Freshness::Current};
        snapshot.notices.push_back(std::move(notice));
        backend::ActivitySnapshot activity;
        activity.key = "activity-1";
        activity.subjectId = "subject-1";
        activity.kind = "test";
        activity.lifecycle = "running";
        activity.active = true;
        activity.stamp = {9, backend::Freshness::Current};
        snapshot.activities.push_back(std::move(activity));
        snapshot.diagnostics.received = 1;
        return snapshot;
    }

    frontend::Json representativeLegacyState() {
        return {{"threads",
                 frontend::Json::array(
                     {{{"id", "thread-1"},
                       {"cwd", "/PRIVILEGED_THREAD_CWD"},
                       {"extensions", {{"privatePresentation", "PRIVILEGED_THREAD_EXTENSION"}}},
                       {"turns",
                        frontend::Json::array(
                            {{{"id", "turn-1"},
                              {"extensions", {{"privatePresentation", "PRIVILEGED_TURN_EXTENSION"}}},
                              {"items",
                               frontend::Json::array(
                                   {{{"id", "item-1"},
                                     {"type", "commandExecution"},
                                     {"commandOutput", "PRIVILEGED_COMMAND_OUTPUT"},
                                     {"data",
                                      {{"command", "PRIVILEGED_COMMAND"},
                                       {"cwd", "/PRIVILEGED_CWD"},
                                       {"changes", frontend::Json::array({{{"path", "/PRIVILEGED_PATH"}, {"diff", "PRIVILEGED_DIFF"}}})}}},
                                     {"extensions", {{"privatePresentation", "PRIVILEGED_ITEM_EXTENSION"}}}}})}}})}}})},
                {"pendingRequests", frontend::Json::array()},
                {"codexExtensions", frontend::Json::array()},
                {"safe", true}};
    }

    frontend::Json expandedSnapshotEnvelope(const detail::SnapshotProjection& projection) {
        return {{"protocol", frontend::ProtocolIdentity},
                {"version", frontend::ProtocolVersion},
                {"kind", frontend::kind::Snapshot},
                {"sequence", projection.snapshot.sequence.value()},
                {"state", projection.snapshot.state}};
    }

    std::string_view notificationMethod(std::string_view registryKey) {
        constexpr std::string_view marker = ":method:";
        const std::size_t offset = registryKey.rfind(marker);
        return offset == std::string_view::npos ? std::string_view{} : registryKey.substr(offset + marker.size());
    }

    std::string_view itemDiscriminator(std::string_view registryKey) {
        constexpr std::string_view marker = ":type:";
        const std::size_t offset = registryKey.rfind(marker);
        return offset == std::string_view::npos ? std::string_view{} : registryKey.substr(offset + marker.size());
    }

    struct BackendItemKindMapping {
        std::string_view backendType;
        frontend::ThreadItemKind frontendKind;
        bool mcpServer = false;
    };

    constexpr std::array BackendItemKindMappings{
        BackendItemKindMapping{"agent_message", frontend::ThreadItemKind::AgentMessage},
        BackendItemKindMapping{"collabAgentToolCall", frontend::ThreadItemKind::CollabAgentToolCall},
        BackendItemKindMapping{"command_execution", frontend::ThreadItemKind::CommandExecution},
        BackendItemKindMapping{"contextCompaction", frontend::ThreadItemKind::ContextCompaction},
        BackendItemKindMapping{"tool_call", frontend::ThreadItemKind::DynamicToolCall},
        BackendItemKindMapping{"enteredReviewMode", frontend::ThreadItemKind::EnteredReviewMode},
        BackendItemKindMapping{"exitedReviewMode", frontend::ThreadItemKind::ExitedReviewMode},
        BackendItemKindMapping{"file_change", frontend::ThreadItemKind::FileChange},
        BackendItemKindMapping{"hookPrompt", frontend::ThreadItemKind::HookPrompt},
        BackendItemKindMapping{"imageGeneration", frontend::ThreadItemKind::ImageGeneration},
        BackendItemKindMapping{"imageView", frontend::ThreadItemKind::ImageView},
        BackendItemKindMapping{"tool_call", frontend::ThreadItemKind::McpToolCall, true},
        BackendItemKindMapping{"plan", frontend::ThreadItemKind::Plan},
        BackendItemKindMapping{"reasoning", frontend::ThreadItemKind::Reasoning},
        BackendItemKindMapping{"sleep", frontend::ThreadItemKind::Sleep},
        BackendItemKindMapping{"subAgentActivity", frontend::ThreadItemKind::SubAgentActivity},
        BackendItemKindMapping{"user_message", frontend::ThreadItemKind::UserMessage},
        BackendItemKindMapping{"web_search", frontend::ThreadItemKind::WebSearch},
    };

    ai::openai::codex::backend::Snapshot itemSnapshot(const BackendItemKindMapping& mapping,
                                                      std::string itemId,
                                                      std::string threadId = "thread-mapping",
                                                      std::string turnId = "turn-mapping") {
        namespace backend = ai::openai::codex::backend;
        backend::ItemSnapshot item;
        item.id = std::move(itemId);
        item.type = std::string(mapping.backendType);
        item.status = "completed";
        item.agentText = "bounded projected text";
        item.data = {{"safeLabel", "projected-from-snapshot"}};
        if (mapping.mcpServer) {
            item.data["server"] = "reviewed-mcp-server";
        }
        item.stamp = {23, backend::Freshness::Current};

        backend::TurnSnapshot turn;
        turn.id = std::move(turnId);
        turn.threadId = threadId;
        turn.status = "completed";
        turn.terminal = true;
        turn.items.push_back(std::move(item));
        turn.stamp = {23, backend::Freshness::Current};

        backend::ThreadSnapshot thread;
        thread.id = std::move(threadId);
        thread.fullyLoaded = true;
        thread.turns.push_back(std::move(turn));
        thread.stamp = {23, backend::Freshness::Current};

        backend::Snapshot snapshot;
        snapshot.provider.lifecycle = backend::ProviderLifecycle::Ready;
        snapshot.provider.generation = 23;
        snapshot.threads.push_back(std::move(thread));
        return snapshot;
    }

    std::string backendPendingType(std::string_view kind) {
        if (kind == "command_execution_approval") {
            return "command_approval";
        }
        if (kind == "file_change_approval") {
            return "file_change_approval";
        }
        return std::string(kind);
    }

    std::set<std::string> stringsFromJsonArray(const frontend::Json& values, std::string_view member) {
        std::set<std::string> result;
        if (!values.is_array()) {
            return result;
        }
        for (const frontend::Json& value : values) {
            if (value.is_object()) {
                const auto iterator = value.find(std::string(member));
                if (iterator != value.end() && iterator->is_string()) {
                    result.insert(iterator->get<std::string>());
                }
            }
        }
        return result;
    }

    void testKnownStructuredSecretRemovalAndBounds(tests::support::TestResult& result) {
        detail::CanonicalSnapshotRecord boundedInput;
        boundedInput.sequence = frontend::SequenceNumber{1};
        boundedInput.legacyState.value = {{"safe", std::string(64, 'x')},
                                          {"accessToken", SecretSentinel},
                                          {"tokenBudget", 4096},
                                          {"tokenUsage", {{"tokensUsed", 17}, {"lifetimeTokens", 29}}},
                                          {"raw", {{"secret", SecretSentinel}}},
                                          {"rawResult", {{"message", SecretSentinel}}},
                                          {"array", frontend::Json::array({1, 2, 3, 4})}};
        boundedInput.expandedState = boundedInput.legacyState;
        detail::FrontendProjectionLimits limits;
        limits.maximumStringBytes = 8;
        limits.maximumArrayItems = 2;
        const detail::CanonicalSnapshotRecord bounded = detail::canonicalizeSnapshot(std::move(boundedInput), limits);

        const bool containsNoKnownStructuredSecrets = detail::canonicalValueContainsNoKnownStructuredSecrets(bounded.legacyState.value) &&
                                                      detail::canonicalValueContainsNoKnownStructuredSecrets(bounded.expandedState.value) &&
                                                      !serializedContainsSecret(bounded.legacyState.value) &&
                                                      !serializedContainsSecret(bounded.expandedState.value);
        result.expectTrue(containsNoKnownStructuredSecrets && bounded.sanitization.knownStructuredSecretFieldsRemoved == 2 &&
                              bounded.sanitization.unsafeRawFieldsRemoved == 4,
                          "canonicalization removes credential fields and raw provider envelopes from both retained representations");
        result.expectTrue(bounded.legacyState.value.at("tokenBudget") == 4096 &&
                              bounded.legacyState.value.at("tokenUsage").at("tokensUsed") == 17 &&
                              bounded.legacyState.value.at("tokenUsage").at("lifetimeTokens") == 29,
                          "canonicalization preserves non-secret token budgets, usage, and accounting counters");
        result.expectTrue(bounded.sanitization.truncated && bounded.sanitization.stringsTruncated == 2 &&
                              bounded.legacyState.value.at("safe").get<std::string>().size() == 8 &&
                              bounded.legacyState.value.at("array").size() == 2,
                          "canonicalization deterministically bounds strings and arrays before journal retention");
    }

    void testKnownStructuredSecretsAndPotentiallySensitiveText(tests::support::TestResult& result) {
        const frontend::Json inputValue{
            {"accessToken", SecretSentinel},
            {"rawProviderEnvelope", {{"payload", SecretSentinel}}},
            {"commandOutput", ArbitraryTextSentinel},
        };
        const std::vector<detail::ScopeProjectionRule> rules{
            {"/commandOutput", {frontend::FrontendScope::CommandExecution}, detail::ScopeProjectionAction::Omit},
        };

        detail::CanonicalSnapshotRecord snapshotInput;
        snapshotInput.sequence = frontend::SequenceNumber{9};
        snapshotInput.legacyState = {inputValue, rules};
        snapshotInput.expandedState = {inputValue, rules};
        const detail::CanonicalSnapshotRecord snapshot = detail::canonicalizeSnapshot(std::move(snapshotInput));

        const bool structuredFieldsRemoved =
            !snapshot.legacyState.value.contains("accessToken") && !snapshot.legacyState.value.contains("rawProviderEnvelope") &&
            !snapshot.expandedState.value.contains("accessToken") && !snapshot.expandedState.value.contains("rawProviderEnvelope");
        const bool arbitraryTextRetained = snapshot.legacyState.value.value("commandOutput", "") == ArbitraryTextSentinel &&
                                           snapshot.expandedState.value.value("commandOutput", "") == ArbitraryTextSentinel;
        result.expectTrue(structuredFieldsRemoved && arbitraryTextRetained &&
                              detail::canonicalValueContainsNoKnownStructuredSecrets(snapshot.legacyState.value) &&
                              detail::canonicalValueContainsNoKnownStructuredSecrets(snapshot.expandedState.value),
                          "canonical retention removes known structured credentials and unsafe raw envelopes while preserving bounded "
                          "potentially sensitive arbitrary command-output text");

        const auto localSnapshot = detail::projectSnapshot(snapshot, localExpandedContext());
        const auto remoteSnapshot = detail::projectSnapshot(snapshot, defaultExpandedContext());
        const auto remoteSnapshotWithoutMetadata = detail::projectSnapshot(snapshot, defaultExpandedContext(false));
        result.expectTrue(localSnapshot && localSnapshot->snapshot.state.value("commandOutput", "") == ArbitraryTextSentinel &&
                              remoteSnapshot && !remoteSnapshot->snapshot.state.contains("commandOutput") &&
                              remoteSnapshotWithoutMetadata && !remoteSnapshotWithoutMetadata->snapshot.state.contains("commandOutput"),
                          "snapshot projection exposes potentially sensitive command output only to CommandExecution scope, independent "
                          "of omission-metadata negotiation");

        detail::CanonicalEventRecord eventInput;
        eventInput.sequence = frontend::SequenceNumber{10};
        eventInput.legacyType = "codex.extension";
        eventInput.legacyData = {inputValue, rules};
        eventInput.requiredScopes = {frontend::FrontendScope::CommandExecution};
        eventInput.expandedEvents = {
            {frontend::ExpandedEventType::ProcessUpdated, {inputValue, rules}, {}},
        };
        const detail::CanonicalEventRecord event = detail::canonicalizeEvent(std::move(eventInput));
        const auto retainedBytes = detail::canonicalEventRetainedBytes(event);
        const detail::EventProjection localLive = detail::projectEvent(event, localExpandedContext());
        const detail::EventProjection localReplay = detail::projectEvent(event, localExpandedContext(), frontend::SequenceNumber{9});
        const detail::EventProjection remoteLive = detail::projectEvent(event, defaultExpandedContext());
        const detail::EventProjection remoteReplay = detail::projectEvent(event, defaultExpandedContext(), frontend::SequenceNumber{9});
        result.expectTrue(retainedBytes.has_value() && localLive.events.size() == 1 && localReplay.events == localLive.events &&
                              localLive.events.front().data.value("commandOutput", "") == ArbitraryTextSentinel &&
                              remoteLive.events.empty() && remoteReplay.events.empty(),
                          "live and replay projections apply the snapshot information ceiling to the same retained potentially "
                          "sensitive command-output text");
    }

    void testSnapshotProjection(tests::support::TestResult& result) {
        const detail::CanonicalSnapshotRecord record = canonicalSnapshot();
        const auto local = detail::projectSnapshot(record, localExpandedContext());
        const auto remote = detail::projectSnapshot(record, defaultExpandedContext());
        const auto remoteWithoutMetadata = detail::projectSnapshot(record, defaultExpandedContext(false));

        const bool canonicalRetainsPrivilege = record.expandedState.value.contains("processes") &&
                                               record.expandedState.value.contains("filesystemWatches") &&
                                               record.expandedState.value.at("items").at(0).contains("commandOutput");
        const bool localReceivesPrivilege =
            local && local->snapshot.state.contains("processes") && local->snapshot.state.contains("filesystemWatches") &&
            local->snapshot.state.at("items").at(0).contains("commandOutput") && local->snapshot.state.at("publicNote") == "visible";
        const bool pendingPresentationIsSafe =
            record.expandedState.value.at("pendingRequests").at(0).at("details").at("summary") == "safe approval summary" &&
            !record.expandedState.value.at("pendingRequests").at(0).at("details").contains("accessToken");
        const bool remoteIsFiltered =
            remote && !remote->snapshot.state.contains("processes") && !remote->snapshot.state.contains("filesystemWatches") &&
            remote->snapshot.state.contains("pendingRequests") &&
            remote->snapshot.state.at("pendingRequests").at(0).at("details").at("summary") == "safe approval summary" &&
            !remote->snapshot.state.at("items").at(0).contains("commandOutput") && remote->snapshot.state.at("publicNote") == "[redacted]";
        result.expectTrue(canonicalRetainsPrivilege && localReceivesPrivilege && pendingPresentationIsSafe && remoteIsFiltered,
                          "one canonical snapshot retains bounded potentially sensitive privileged state and projects it by principal "
                          "scope");

        result.expectTrue(local && remote && local->snapshot.sequence == record.sequence && remote->snapshot.sequence == record.sequence &&
                              local->expanded && remote->expanded,
                          "local and default projections retain the canonical snapshot sequence and negotiated expanded representation");
        result.expectTrue(local && local->snapshot.state.at("threadList").at("hasLoadedPage") == true &&
                              local->snapshot.state.at("threadList").at("complete") == false &&
                              local->snapshot.state.at("threadList").at("pagesLoaded") == 3 &&
                              local->snapshot.state.at("threadList").at("nextCursor") == "next-thread-page" &&
                              local->snapshot.state.at("threadList").at("backwardsCursor") == "previous-thread-page" &&
                              local->snapshot.state.at("threadList").at("stamp").at("generation") == 9,
                          "expanded snapshots carry authoritative thread-list paging, cursor, and source-stamp state");

        const bool metadataOnlyDifference =
            remote && remoteWithoutMetadata && remote->snapshot.state == remoteWithoutMetadata->snapshot.state &&
            remote->snapshot.extensions.contains("scopeProjection") &&
            !remoteWithoutMetadata->snapshot.extensions.contains("scopeProjection") && !remote->omittedFields.empty();
        result.expectTrue(metadataOnlyDifference,
                          "scope_projected_state changes omission metadata only and cannot increase the projected information ceiling");

        const frontend::FrontendPrincipal noObserver = principal("no-observe", std::array{frontend::FrontendScope::Control}, "no_observe");
        result.expectTrue(!detail::projectSnapshot(record, detail::makeProjectionContext(noObserver)).has_value(),
                          "a principal lacking Observe receives no canonical snapshot projection");
        result.expectTrue(!serializedContainsSecret(record.legacyState.value) && !serializedContainsSecret(record.expandedState.value) &&
                              local && !serializedContainsSecret(local->snapshot.state) && remote &&
                              !serializedContainsSecret(remote->snapshot.state),
                          "known structured-secret sentinels are absent from canonical, local, and default snapshot forms");
    }

    void testProjectionResourceBounds(tests::support::TestResult& result) {
        detail::CanonicalSnapshotRecord visitInput;
        visitInput.sequence = frontend::SequenceNumber{2};
        visitInput.legacyState.value = {{"a", 1}, {"b", 2}, {"c", 3}};
        visitInput.expandedState.value = visitInput.legacyState.value;
        visitInput.extensions = {{"safe", true}};
        detail::FrontendProjectionLimits visitLimits;
        visitLimits.maximumVisits = 3;
        const detail::CanonicalSnapshotRecord visitBounded = detail::canonicalizeSnapshot(std::move(visitInput), visitLimits);
        result.expectTrue(visitBounded.sanitization.visits == 3 && visitBounded.sanitization.truncated,
                          "one visit budget bounds the complete canonical record rather than restarting for each representation");

        detail::CanonicalSnapshotRecord rulesInput;
        rulesInput.sequence = frontend::SequenceNumber{3};
        rulesInput.legacyState.value = frontend::Json::object();
        rulesInput.expandedState.value = {
            {"entries", frontend::Json::array({frontend::Json{{"value", "first"}}, frontend::Json{{"value", "second"}}})}};
        rulesInput.expandedState.rules = {
            {"/entries/1", {frontend::FrontendScope::AccountManagement}, detail::ScopeProjectionAction::Omit}};
        const detail::CanonicalSnapshotRecord rulesRecord = detail::canonicalizeSnapshot(std::move(rulesInput));
        const auto projected = detail::projectSnapshot(rulesRecord, defaultExpandedContext());
        result.expectTrue(projected && projected->snapshot.state.at("entries").size() == 1 &&
                              projected->snapshot.state.at("entries").front().at("value") == "first" &&
                              projected->omittedFields == std::vector<std::string>{"/entries/1"},
                          "scope rules can omit an entire array element without leaving a placeholder or traversing its value");

        detail::CanonicalSnapshotRecord identityInput;
        identityInput.sequence = frontend::SequenceNumber{31};
        identityInput.legacyState.value = frontend::Json::object();
        identityInput.expandedState.value = {
            {"items", frontend::Json::array({frontend::Json{{"id", "stable-item"}, {"type", "plan"}}, frontend::Json{{"type", "plan"}}})}};
        const detail::CanonicalSnapshotRecord identityRecord = detail::canonicalizeSnapshot(std::move(identityInput));
        const auto identityProjected = detail::projectSnapshot(
            identityRecord,
            localContext({frontend::FrontendCapability::CompleteBackendDomains, frontend::FrontendCapability::ScopeProjectedState}));
        result.expectTrue(identityProjected && identityProjected->snapshot.state.at("items").size() == 1 &&
                              identityProjected->snapshot.state.at("items").front().at("id") == "stable-item" &&
                              std::find(identityProjected->omittedFields.begin(), identityProjected->omittedFields.end(), "/items/1") !=
                                  identityProjected->omittedFields.end(),
                          "metadata-compatible snapshot projection omits an identity-less item instead of fabricating an ID");
    }

    void testIndependentSnapshotCapabilities(tests::support::TestResult& result) {
        detail::CanonicalSnapshotRecord input;
        input.sequence = frontend::SequenceNumber{4};
        input.legacyState.value = {{"legacy", true},
                                   {"items", frontend::Json::array({frontend::Json{{"id", "item-1"}, {"codexType", "plan"}}})},
                                   {"pendingRequests", frontend::Json::array({frontend::Json{{"pendingRequestId", "8"}}})}};
        input.expandedState.value = {
            {"provider", frontend::Json::object()},
            {"items", frontend::Json::array({frontend::Json{{"id", "item-1"}, {"type", "plan"}, {"status", "completed"}}})},
            {"pendingRequests",
             frontend::Json::array(
                 {frontend::Json{{"pendingRequestId", "8"}, {"kind", "command_execution_approval"}, {"summary", "safe"}}})}};
        const detail::CanonicalSnapshotRecord record = detail::canonicalizeSnapshot(std::move(input));
        const auto legacy = detail::projectSnapshot(record, localContext({}));
        const auto domains = detail::projectSnapshot(record, localContext({frontend::FrontendCapability::CompleteBackendDomains}));
        const auto items = detail::projectSnapshot(record, localContext({frontend::FrontendCapability::CompleteThreadItems}));
        const auto pending = detail::projectSnapshot(record, localContext({frontend::FrontendCapability::DedicatedPendingRequests}));
        const auto all = detail::projectSnapshot(record,
                                                 localContext({frontend::FrontendCapability::CompleteBackendDomains,
                                                               frontend::FrontendCapability::CompleteThreadItems,
                                                               frontend::FrontendCapability::DedicatedPendingRequests}));

        const bool legacyOnly = legacy && !legacy->expanded && legacy->snapshot.state.at("legacy") &&
                                legacy->snapshot.state.at("items").at(0).contains("codexType") &&
                                !legacy->snapshot.state.at("pendingRequests").at(0).contains("kind");
        const bool domainsOnly = domains && domains->expanded && domains->snapshot.state.contains("provider") &&
                                 domains->snapshot.state.at("items").at(0).contains("codexType") &&
                                 !domains->snapshot.state.at("items").at(0).contains("status") &&
                                 !domains->snapshot.state.contains("pendingRequests");
        const bool itemsOnly = items && !items->expanded && items->snapshot.state.at("legacy") &&
                               items->snapshot.state.at("items").at(0).at("type") == "plan" &&
                               !items->snapshot.state.at("pendingRequests").at(0).contains("kind");
        const bool pendingOnly = pending && !pending->expanded && pending->snapshot.state.at("legacy") &&
                                 pending->snapshot.state.at("items").at(0).contains("codexType") &&
                                 pending->snapshot.state.at("pendingRequests").at(0).at("kind") == "command_execution_approval";
        const bool allExpanded = all && all->expanded && all->snapshot.state.contains("provider") &&
                                 all->snapshot.state.at("items").at(0).at("type") == "plan" &&
                                 all->snapshot.state.at("pendingRequests").at(0).at("kind") == "command_execution_approval";
        result.expectTrue(legacyOnly && domainsOnly && itemsOnly && pendingOnly && allExpanded,
                          "complete domains, complete ThreadItems, and dedicated pending requests compose independently");
    }

    void testEventProjectionAndReuse(tests::support::TestResult& result) {
        const detail::CanonicalEventRecord record = canonicalEvent();
        const detail::EventProjection localLive = detail::projectEvent(record, localExpandedContext());
        const detail::EventProjection localReplay = detail::projectEvent(record, localExpandedContext());
        const detail::EventProjection replayBefore = detail::projectEvent(record, localExpandedContext(), frontend::SequenceNumber{41});
        const detail::EventProjection replayAfter = detail::projectEvent(record, localExpandedContext(), frontend::SequenceNumber{42});
        const detail::EventProjection remote = detail::projectEvent(record, defaultExpandedContext());
        const detail::EventProjection remoteWithoutMetadata = detail::projectEvent(record, defaultExpandedContext(false));

        const bool sameRecordAndSequence = localLive.events == localReplay.events && localLive.events.size() == 2 &&
                                           localLive.events[0].sequence.value() == 42 && localLive.events[1].sequence.value() == 42 &&
                                           remote.events.size() == 1 && remote.events[0].sequence.value() == 42;
        const bool cursorExact = replayBefore.events == localLive.events && replayAfter.events.empty();
        result.expectTrue(sameRecordAndSequence && cursorExact,
                          "one canonical occurrence owns one sequence across local, reduced-scope, live, and replay projections");

        const auto remoteProcess = std::find_if(remote.events.begin(), remote.events.end(), [](const auto& event) {
            return event.type == "process.updated";
        });
        const bool privilegedFieldsLocalOnly = localLive.events.size() == 2 && localLive.events.front().type == "process.updated" &&
                                               localLive.events.front().data.contains("process") &&
                                               localLive.events.front().data.at("process").contains("command") &&
                                               remoteProcess == remote.events.end();
        result.expectTrue(privilegedFieldsLocalOnly,
                          "bounded privileged process state remains canonical while its event family is scope-omitted");

        const bool metadataOnlyDifference = remote.events.size() == remoteWithoutMetadata.events.size() && !remote.events.empty() &&
                                            remote.events.front().data == remoteWithoutMetadata.events.front().data &&
                                            remote.events.front().type == remoteWithoutMetadata.events.front().type &&
                                            remote.events.front().extensions.contains("scopeProjection") &&
                                            !remoteWithoutMetadata.events.front().extensions.contains("scopeProjection");
        result.expectTrue(metadataOnlyDifference,
                          "event scope_projected_state negotiation adds omission metadata without changing event data");

        bool expandedEventsSchemaValid = true;
        for (const frontend::FrontendEvent& event : localLive.events) {
            const auto type = frontend::expandedEventTypeFromString(event.type);
            const auto encoded = type ? frontend::Codec::encodeExpandedEvent(
                                            frontend::ExpandedFrontendEvent{event.sequence, *type, event.data, event.extensions})
                                      : frontend::CodecResult<frontend::Json>{frontend::CodecError{}};
            expandedEventsSchemaValid = expandedEventsSchemaValid && encoded.hasValue();
        }
        result.expectTrue(expandedEventsSchemaValid,
                          "projected dedicated events remain valid complete Frontend Protocol v1 expanded-event values");

        const detail::FrontendProjectionContext legacyLocal = detail::makeProjectionContext(
            principal("legacy-local", frontend::LocalTrustedScopes, std::string(frontend::LocalTrustedScopeProfile.name)));
        const detail::EventProjection legacy = detail::projectEvent(record, legacyLocal);
        const detail::EventProjection legacyRemote = detail::projectEvent(record, defaultContext({}));
        result.expectTrue(legacy.events.size() == 1 && legacy.events.front().type == "codex.extension" &&
                              legacy.events.front().sequence.value() == 42 && legacy.events.front().data.contains("delta") &&
                              legacyRemote.events.size() == 1 && !legacyRemote.events.front().data.contains("delta"),
                          "legacy local and reduced-scope connections share one compatibility occurrence while mandatory filtering removes "
                          "privileged data");

        const detail::CanonicalEventRecord configWarning = detail::makeCanonicalEventRecord(
            "codex.extension",
            frontend::Json{
                {"method", "configWarning"},
                {"params",
                 {{"summary", "safe warning"}, {"details", "safe details"}, {"path", "/private/config.toml"}, {"range", nullptr}}}},
            representativeBackendSnapshot(),
            frontend::SequenceNumber{43});
        const detail::EventProjection localLegacyConfig = detail::projectEvent(configWarning, legacyLocal);
        const detail::EventProjection remoteLegacyConfig = detail::projectEvent(configWarning, defaultContext({}));
        const detail::EventProjection remoteExpandedConfig = detail::projectEvent(configWarning, defaultExpandedContext());
        result.expectTrue(
            localLegacyConfig.events.size() == 1 &&
                localLegacyConfig.events.front().data.at("params").at("path") == "/private/config.toml" &&
                remoteLegacyConfig.events.size() == 1 && !remoteLegacyConfig.events.front().data.contains("params") &&
                remoteExpandedConfig.events.size() == 2 && remoteExpandedConfig.events.front().type == "configuration.updated" &&
                remoteExpandedConfig.events.back().type == "notice.added",
            "notice compatibility never bypasses privileged legacy payload filtering for configWarning paths "
            "(local=" +
                (localLegacyConfig.events.empty() ? std::string("[]") : localLegacyConfig.events.front().data.dump()) +
                ", remote=" + (remoteLegacyConfig.events.empty() ? std::string("[]") : remoteLegacyConfig.events.front().data.dump()) +
                ", expanded-count=" + std::to_string(remoteExpandedConfig.events.size()) + ")");

        const detail::CanonicalEventRecord processOnly = detail::makeCanonicalEventRecord(
            "codex.extension",
            frontend::Json{{"method", "process/outputDelta"},
                           {"params", {{"processId", "process-1"}, {"delta", "bounded privileged output"}}}},
            representativeBackendSnapshot(),
            frontend::SequenceNumber{44});
        const detail::EventProjection remoteLegacyProcess = detail::projectEvent(processOnly, defaultContext({}));
        const detail::EventProjection remoteExpandedProcess = detail::projectEvent(processOnly, defaultExpandedContext());
        result.expectTrue(remoteLegacyProcess.events.empty() && remoteExpandedProcess.events.empty(),
                          "capability omission cannot expose a privileged-only process occurrence hidden from expanded projection");

        const bool reusableAfterFiltering = detail::projectEvent(record, localExpandedContext()).events == localLive.events;
        result.expectTrue(
            reusableAfterFiltering && detail::canonicalValueContainsNoKnownStructuredSecrets(record.legacyData.value) &&
                std::all_of(record.expandedEvents.begin(),
                            record.expandedEvents.end(),
                            [](const auto& event) {
                                return detail::canonicalValueContainsNoKnownStructuredSecrets(event.data.value);
                            }) &&
                std::none_of(localLive.events.begin(),
                             localLive.events.end(),
                             [](const auto& event) {
                                 return serializedContainsSecret(event.data) || serializedContainsSecret(event.extensions);
                             }),
            "projection failures or reduced scopes do not mutate the canonical record, and later projections retain no known structured "
            "secrets");
    }

    void testBackendProjectionBuilderAndGeneratedMappings(tests::support::TestResult& result) {
        namespace backend = ai::openai::codex::backend;
        const backend::Snapshot emptyBackend;
        const detail::CanonicalSnapshotRecord emptyRecord =
            detail::makeCanonicalSnapshotRecord(frontend::Json::object(), emptyBackend, frontend::SequenceNumber{70});
        const auto emptyProjection = detail::projectSnapshot(emptyRecord, localExpandedContext());
        const auto emptyDecoded = emptyProjection ? frontend::Codec::decodeExpandedSnapshot(expandedSnapshotEnvelope(*emptyProjection))
                                                  : frontend::CodecResult<frontend::ExpandedSnapshot>{frontend::CodecError{}};
        const bool emptySchemaValid = emptyProjection && emptyDecoded.hasValue();

        const backend::Snapshot snapshot = representativeBackendSnapshot();
        frontend::Json legacyState = representativeLegacyState();
        legacyState["accessToken"] = SecretSentinel;
        const detail::CanonicalSnapshotRecord record =
            detail::makeCanonicalSnapshotRecord(std::move(legacyState), snapshot, frontend::SequenceNumber{71});
        const auto local = detail::projectSnapshot(record, localExpandedContext());
        const auto remote = detail::projectSnapshot(record, defaultExpandedContext());
        const auto localDecoded = local ? frontend::Codec::decodeExpandedSnapshot(expandedSnapshotEnvelope(*local))
                                        : frontend::CodecResult<frontend::ExpandedSnapshot>{frontend::CodecError{}};
        const auto remoteDecoded = remote ? frontend::Codec::decodeExpandedSnapshot(expandedSnapshotEnvelope(*remote))
                                          : frontend::CodecResult<frontend::ExpandedSnapshot>{frontend::CodecError{}};
        const bool localSchemaValid = local && localDecoded.hasValue();
        const bool remoteSchemaValid = remote && remoteDecoded.hasValue();
        const bool projectionCeilings =
            local && remote && local->snapshot.state.contains("processes") && !remote->snapshot.state.contains("processes") &&
            local->snapshot.state.at("processes").at("entries").at(0).contains("processHandle") &&
            local->snapshot.state.contains("filesystemWatches") && !remote->snapshot.state.contains("filesystemWatches") &&
            local->snapshot.state.at("filesystemWatches").at("entries").at(0).contains("watchId") &&
            !serializedContainsSecret(record.legacyState.value) && !serializedContainsSecret(record.expandedState.value) &&
            !serializedContainsSecret(local->snapshot.state) && !serializedContainsSecret(remote->snapshot.state);
        const auto localLegacy = detail::projectSnapshot(
            record,
            detail::makeProjectionContext(
                principal("local-legacy", frontend::LocalTrustedScopes, std::string(frontend::LocalTrustedScopeProfile.name))));
        const auto remoteLegacy = detail::projectSnapshot(
            record,
            detail::makeProjectionContext(
                principal("remote-legacy", frontend::DefaultRemoteScopes, std::string(frontend::DefaultRemoteScopeProfile.name))));
        const bool legacyCeiling =
            localLegacy && remoteLegacy && localLegacy->snapshot.state.dump().find("PRIVILEGED_") != std::string::npos &&
            remoteLegacy->snapshot.state.dump().find("PRIVILEGED_") == std::string::npos &&
            remoteLegacy->snapshot.state.at("threads").at(0).at("turns").at(0).at("items").at(0).at("id") == "item-1";
        result.expectTrue(emptySchemaValid,
                          "BackendProjectionBuilder empty expanded snapshot is schema-valid" +
                              (emptyDecoded ? std::string{} : ": " + emptyDecoded.error().message));
        result.expectTrue(localSchemaValid,
                          "BackendProjectionBuilder local_trusted expanded snapshot is schema-valid" +
                              (localDecoded ? std::string{} : ": " + localDecoded.error().message));
        result.expectTrue(remoteSchemaValid,
                          "BackendProjectionBuilder default_remote expanded snapshot is schema-valid" +
                              (remoteDecoded ? std::string{} : ": " + remoteDecoded.error().message));
        result.expectTrue(projectionCeilings,
                          "BackendProjectionBuilder removes known structured-secret fields and applies one local/default scope ceiling to "
                          "representative domains");
        result.expectTrue(legacyCeiling,
                          "legacy snapshots retain authorized privileged state locally and filter nested command/filesystem data remotely");

        bool everyNotificationMapped = true;
        bool everyProjectedEventSchemaValid = true;
        bool noDuplicateFamily = true;
        bool exactOccurrenceSequences = true;
        bool exactlyOneCompatibilityRepresentation = true;
        std::set<std::string> mappedFamilies;
        const std::set<std::string> expectedNotificationFamilies{
            "account.updated",
            "configuration.updated",
            "diagnostics.updated",
            "filesystemWatch.updated",
            "fuzzySearch.updated",
            "integrations.updated",
            "item.content.updated",
            "item.upserted",
            "mcp.updated",
            "notice.added",
            "pendingRequests.updated",
            "platform.updated",
            "process.updated",
            "reviews.updated",
            "skills.updated",
            "thread.removed",
            "thread.upserted",
            "turn.upserted",
        };
        for (std::size_t index = 0; index < generated::AllNotificationProjections.size(); ++index) {
            const generated::ProjectionMetadata& metadata = generated::AllNotificationProjections[index];
            const std::string_view method = notificationMethod(metadata.registryKey);
            frontend::Json params{{"threadId", "thread-1"},
                                  {"turnId", "turn-1"},
                                  {"itemId", "item-1"},
                                  {"processHandle", "process-1"},
                                  {"processId", "process-1"},
                                  {"watchId", "watch-1"},
                                  {"sessionId", "search-1"},
                                  {"summary", "exact projected notice"},
                                  {"message", "exact projected warning"},
                                  {"details", "exact projected details"},
                                  {"failedScan", false},
                                  {"samplePaths", frontend::Json::array()},
                                  {"extraCount", 0}};
            params["thread"] = frontend::Json{{"id", "thread-1"}};
            params["turn"] = frontend::Json{{"id", "turn-1"}, {"threadId", "thread-1"}};
            params["item"] = frontend::Json{{"id", "item-1"}};
            detail::CanonicalEventRecord eventRecord = detail::makeCanonicalEventRecord(
                "codex.extension",
                frontend::Json{{"method", method}, {"params", std::move(params)}, {"accessToken", SecretSentinel}},
                snapshot,
                frontend::SequenceNumber{100 + index});
            std::vector<std::string_view> families;
            for (const detail::CanonicalExpandedEvent& event : eventRecord.expandedEvents) {
                families.push_back(frontend::toString(event.type));
                mappedFamilies.emplace(frontend::toString(event.type));
            }
            std::sort(families.begin(), families.end());
            noDuplicateFamily = noDuplicateFamily && std::adjacent_find(families.begin(), families.end()) == families.end();
            everyNotificationMapped =
                everyNotificationMapped && eventRecord.registryKey == metadata.registryKey && !eventRecord.expandedEvents.empty() &&
                families.size() == metadata.expandedMappings.size() &&
                std::all_of(metadata.expandedMappings.begin(), metadata.expandedMappings.end(), [&families](std::string_view family) {
                    return std::binary_search(families.begin(), families.end(), family);
                });
            const detail::EventProjection projected = detail::projectEvent(eventRecord, localExpandedContext());
            everyNotificationMapped = everyNotificationMapped && projected.events.size() == metadata.expandedMappings.size();
            for (const frontend::FrontendEvent& event : projected.events) {
                exactOccurrenceSequences = exactOccurrenceSequences && event.sequence.value() == 100 + index;
                const auto type = frontend::expandedEventTypeFromString(event.type);
                everyProjectedEventSchemaValid = everyProjectedEventSchemaValid && type.has_value() &&
                                                 frontend::Codec::encodeExpandedEvent(
                                                     frontend::ExpandedFrontendEvent{event.sequence, *type, event.data, event.extensions})
                                                     .hasValue();
            }
            const detail::EventProjection legacy = detail::projectEvent(eventRecord, localContext({}));
            exactlyOneCompatibilityRepresentation = exactlyOneCompatibilityRepresentation && legacy.events.size() == 1 &&
                                                    legacy.events.front().type == "codex.extension" &&
                                                    legacy.events.front().sequence.value() == 100 + index;
        }
        result.expectTrue(
            everyNotificationMapped && everyProjectedEventSchemaValid && noDuplicateFamily && exactOccurrenceSequences &&
                exactlyOneCompatibilityRepresentation && mappedFamilies == expectedNotificationFamilies,
            "all 68 notification mappings cover 18 dedicated families with one canonical sequence and one legacy representation");

        backend::Snapshot multiThreadSnapshot = snapshot;
        backend::ThreadSnapshot secondThread;
        secondThread.id = "thread-2";
        secondThread.title = "Second projection thread";
        secondThread.fullyLoaded = true;
        secondThread.stamp = {9, backend::Freshness::Current};
        multiThreadSnapshot.threads.push_back(std::move(secondThread));
        const detail::CanonicalEventRecord multiThreadRecord = detail::makeCanonicalEventRecord(
            "thread.list.updated", frontend::Json::object(), multiThreadSnapshot, frontend::SequenceNumber{199});
        const detail::EventProjection multiThreadProjection = detail::projectEvent(multiThreadRecord, localExpandedContext());
        const detail::EventProjection multiThreadReplay =
            detail::projectEvent(multiThreadRecord, localExpandedContext(), frontend::SequenceNumber{198});
        const detail::EventProjection multiThreadAlreadyApplied =
            detail::projectEvent(multiThreadRecord, localExpandedContext(), frontend::SequenceNumber{199});
        const detail::EventProjection multiThreadLegacy = detail::projectEvent(multiThreadRecord, localContext({}));
        const frontend::EventBatch multiThreadBatch{
            frontend::SequenceNumber{199}, frontend::SequenceNumber{199}, multiThreadProjection.events, frontend::Json::object()};
        result.expectTrue(multiThreadProjection.events.size() == 1 && multiThreadProjection.events.front().type == "threadList.updated" &&
                              multiThreadProjection.events.front().sequence == frontend::SequenceNumber{199} &&
                              multiThreadProjection.events.front().data.at("threadList").at("pagesLoaded") == 3 &&
                              multiThreadReplay.events == multiThreadProjection.events && multiThreadReplay.expanded &&
                              multiThreadAlreadyApplied.events.empty() && multiThreadLegacy.events.size() == 1 &&
                              multiThreadLegacy.events.front().type == "thread.list.updated" &&
                              frontend::Codec::encodeServer(frontend::ServerMessage{multiThreadBatch}).hasValue(),
                          "live and replay select the same compact thread-list event, while the replay cursor suppresses an already "
                          "represented occurrence and legacy selects exactly one compatibility representation");

        backend::Snapshot emptyThreadSnapshot = snapshot;
        emptyThreadSnapshot.threads.clear();
        const detail::CanonicalEventRecord emptyThreadRecord = detail::makeCanonicalEventRecord(
            "thread.list.updated", frontend::Json::object(), emptyThreadSnapshot, frontend::SequenceNumber{200});
        const detail::EventProjection emptyThreadProjection = detail::projectEvent(emptyThreadRecord, localExpandedContext());
        result.expectTrue(emptyThreadProjection.events.size() == 1 && emptyThreadProjection.events.front().type == "threadList.updated" &&
                              !emptyThreadProjection.events.front().data.contains("thread"),
                          "an empty thread-list page emits one metadata event and fabricates no thread");
    }

    void testAllThreadItemRuntimeMappings(tests::support::TestResult& result) {
        namespace backend = ai::openai::codex::backend;
        backend::Snapshot snapshot;
        backend::TurnSnapshot turn;
        turn.id = "turn-all-items";
        turn.threadId = "thread-all-items";
        turn.status = "completed";
        turn.terminal = true;
        turn.stamp = {17, backend::Freshness::Current};

        std::set<std::string> expectedTypes;
        for (std::size_t index = 0; index < generated::AllThreadItemProjections.size(); ++index) {
            const std::string_view type = itemDiscriminator(generated::AllThreadItemProjections[index].registryKey);
            expectedTypes.emplace(type);
            backend::ItemSnapshot item;
            item.id = "item-" + std::to_string(index);
            item.type = std::string(type);
            item.status = "completed";
            item.agentText = "bounded agent text";
            item.reasoningSummary = "bounded reasoning summary";
            item.commandOutput = type == "commandExecution" ? "bounded command output" : std::string{};
            item.data = {{"safeLabel", "safe-" + std::to_string(index)}, {"accessToken", SecretSentinel}};
            item.stamp = {17, backend::Freshness::Current};
            turn.items.push_back(std::move(item));
        }
        backend::ThreadSnapshot thread;
        thread.id = turn.threadId;
        thread.title = "All item alternatives";
        thread.fullyLoaded = true;
        thread.stamp = {17, backend::Freshness::Current};
        thread.turns.push_back(std::move(turn));
        snapshot.threads.push_back(std::move(thread));

        const detail::CanonicalSnapshotRecord record = detail::makeCanonicalSnapshotRecord(
            frontend::Json{{"items", frontend::Json::array()}}, snapshot, frontend::SequenceNumber{210});
        const auto local = detail::projectSnapshot(record, localExpandedContext());
        const auto remote = detail::projectSnapshot(record, defaultExpandedContext());
        const auto metadataCompatible =
            detail::projectSnapshot(record, localContext({frontend::FrontendCapability::CompleteBackendDomains}));

        bool allUsefulAndSafe = local && local->snapshot.state.at("items").size() == expectedTypes.size() &&
                                stringsFromJsonArray(local->snapshot.state.at("items"), "type") == expectedTypes;
        bool exactLocations = allUsefulAndSafe;
        if (local) {
            for (const frontend::Json& item : local->snapshot.state.at("items")) {
                allUsefulAndSafe = allUsefulAndSafe && item.contains("id") && item.contains("type") && item.contains("status") &&
                                   item.at("status") == "completed" && item.contains("generation") && item.at("generation") == 17 &&
                                   item.contains("freshness") && item.at("freshness") == "current" && item.contains("data") &&
                                   item.at("data").contains("safeLabel") && !item.at("data").contains("accessToken") &&
                                   item.contains("agentText") && item.at("agentText") == "bounded agent text" &&
                                   item.contains("reasoningSummary") && item.at("reasoningSummary") == "bounded reasoning summary";
                exactLocations = exactLocations && item.contains("threadId") && item.at("threadId") == "thread-all-items" &&
                                 item.contains("turnId") && item.at("turnId") == "turn-all-items";
            }
        }

        bool remoteKeepsItemsButFiltersOutput = remote && remote->snapshot.state.at("items").size() == expectedTypes.size();
        if (remote) {
            for (const frontend::Json& item : remote->snapshot.state.at("items")) {
                remoteKeepsItemsButFiltersOutput = remoteKeepsItemsButFiltersOutput && !item.contains("commandOutput");
            }
        }

        std::size_t normalizedCount = 0;
        std::size_t metadataOnlyCount = 0;
        bool exactCompatibilityForms = metadataCompatible && metadataCompatible->snapshot.state.at("items").size() == expectedTypes.size();
        if (metadataCompatible) {
            for (const frontend::Json& item : metadataCompatible->snapshot.state.at("items")) {
                const auto* metadata =
                    detail::threadItemProjection("item_discriminator:ThreadItem:type:" + item.at("type").get<std::string>());
                exactCompatibilityForms = exactCompatibilityForms && metadata != nullptr;
                if (metadata != nullptr && metadata->legacyContract == "legacy_metadata_only") {
                    ++metadataOnlyCount;
                    exactCompatibilityForms =
                        exactCompatibilityForms && item.contains("codexType") && !item.contains("status") && !item.contains("data");
                } else {
                    ++normalizedCount;
                    exactCompatibilityForms = exactCompatibilityForms && item.contains("status") && !item.contains("codexType");
                }
            }
        }

        const bool schemaValid = local && remote && frontend::Codec::decodeExpandedSnapshot(expandedSnapshotEnvelope(*local)).hasValue() &&
                                 frontend::Codec::decodeExpandedSnapshot(expandedSnapshotEnvelope(*remote)).hasValue();
        bool allItemEvents = true;
        for (std::size_t index = 0; index < generated::AllThreadItemProjections.size(); ++index) {
            const std::string expectedType(itemDiscriminator(generated::AllThreadItemProjections[index].registryKey));
            const detail::CanonicalEventRecord event = detail::makeCanonicalEventRecord(
                "item.updated",
                frontend::Json{
                    {"threadId", "thread-all-items"}, {"turnId", "turn-all-items"}, {"item", {{"id", "item-" + std::to_string(index)}}}},
                snapshot,
                frontend::SequenceNumber{240 + index});
            const detail::EventProjection projected =
                detail::projectEvent(event, localContext({frontend::FrontendCapability::CompleteThreadItems}));
            allItemEvents = allItemEvents && projected.events.size() == 1 && projected.events.front().type == "item.upserted" &&
                            projected.events.front().data.contains("item") &&
                            projected.events.front().data.at("item").at("type") == expectedType &&
                            projected.events.front().data.at("item").at("id") == "item-" + std::to_string(index) &&
                            !serializedContainsSecret(projected.events.front().data);
        }
        result.expectTrue(
            allUsefulAndSafe && exactLocations && remoteKeepsItemsButFiltersOutput && exactCompatibilityForms && normalizedCount == 8 &&
                metadataOnlyCount == 10 && schemaValid && allItemEvents && !serializedContainsSecret(record.expandedState.value),
            "all 18 ThreadItems retain useful safe fields, stable locations, exact 8/10 compatibility, and scope-filtered content");
    }

    void testBackendItemKindNormalizationAndWireBoundary(tests::support::TestResult& result) {
        namespace backend = ai::openai::codex::backend;

        std::set<frontend::ThreadItemKind> mappedKinds;
        std::set<std::string> mappedSpellings;
        std::set<std::string> generatedSpellings;
        bool everyKindMapped = true;
        bool everyKindRoundTrips = true;
        bool everyItemSchemaValid = true;
        bool everyEventRoundTrips = true;
        bool everyIdentityExact = true;
        bool noRawOccurrenceRepresentation = true;
        bool liveReplayEquivalent = true;
        bool userWireNormalized = false;
        bool agentWireNormalized = false;

        for (const auto& metadata : generated::AllThreadItemProjections) {
            generatedSpellings.emplace(itemDiscriminator(metadata.registryKey));
        }

        for (std::size_t index = 0; index < BackendItemKindMappings.size(); ++index) {
            const BackendItemKindMapping& mapping = BackendItemKindMappings[index];
            const std::string itemId = "item-mapping-" + std::to_string(index);
            backend::Snapshot snapshot = itemSnapshot(mapping, itemId);
            const backend::ItemSnapshot& backendItem = snapshot.threads.front().turns.front().items.front();
            const std::optional<frontend::ThreadItemKind> mapped = detail::expandedItemKind(backendItem);
            const std::string stableType(frontend::toString(mapping.frontendKind));
            const std::optional<frontend::ThreadItemKind> reversed = frontend::threadItemKindFromString(stableType);
            mappedKinds.emplace(mapping.frontendKind);
            mappedSpellings.emplace(stableType);
            everyKindMapped = everyKindMapped && mapped == mapping.frontendKind && !stableType.empty();
            everyKindRoundTrips = everyKindRoundTrips && reversed == mapping.frontendKind;

            const frontend::SequenceNumber sequence{600 + index};
            const detail::CanonicalEventRecord record =
                detail::makeCanonicalEventRecord("item.updated",
                                                 frontend::Json{{"threadId", "thread-mapping"},
                                                                {"turnId", "turn-mapping"},
                                                                {"item",
                                                                 {{"id", itemId},
                                                                  {"type", mapping.backendType},
                                                                  {"rawOccurrenceOnly", "must-not-cross-projection-boundary"},
                                                                  {"data", {{"occurrenceOnly", true}}}}}},
                                                 snapshot,
                                                 sequence);
            const detail::EventProjection live = detail::projectEvent(record, localExpandedContext());
            const detail::EventProjection replay =
                detail::projectEvent(record, localExpandedContext(), frontend::SequenceNumber{sequence.value() - 1});
            liveReplayEquivalent = liveReplayEquivalent && live.events == replay.events && live.expanded == replay.expanded &&
                                   live.omittedFields == replay.omittedFields && live.redactedFields == replay.redactedFields;
            if (live.events.size() != 1 || live.events.front().type != "item.upserted" || !live.events.front().data.contains("item")) {
                everyItemSchemaValid = false;
                everyEventRoundTrips = false;
                everyIdentityExact = false;
                noRawOccurrenceRepresentation = false;
                continue;
            }

            const frontend::Json& projectedItem = live.events.front().data.at("item");
            everyIdentityExact = everyIdentityExact && projectedItem.value("id", "") == itemId &&
                                 projectedItem.value("threadId", "") == "thread-mapping" &&
                                 projectedItem.value("turnId", "") == "turn-mapping";
            noRawOccurrenceRepresentation = noRawOccurrenceRepresentation && projectedItem.value("type", "") == stableType &&
                                            !projectedItem.contains("rawOccurrenceOnly") &&
                                            (!projectedItem.contains("data") || !projectedItem.at("data").contains("occurrenceOnly"));

            const detail::GeneratedSchemaValidation itemValidation = detail::validateGeneratedSchema(
                detail::generatedProtocolSchema(), "#/$defs/ExpandedThreadItem", projectedItem, "expanded item");
            const auto typedEvent = frontend::Codec::encodeExpandedEvent(
                frontend::ExpandedFrontendEvent{sequence, frontend::ExpandedEventType::ItemUpserted, live.events.front().data});
            everyItemSchemaValid = everyItemSchemaValid && itemValidation.valid && typedEvent.hasValue();

            const frontend::EventBatch batch{sequence, sequence, live.events, frontend::Json::object()};
            const auto serialized = frontend::Codec::serializeServer(frontend::ServerMessage{batch});
            bool decodedExact = false;
            std::string itemWire;
            if (serialized) {
                const auto decoded = frontend::Codec::decodeServer(std::string_view{serialized.value()});
                const frontend::Json wire = frontend::Json::parse(serialized.value(), nullptr, false);
                if (!wire.is_discarded() && wire.contains("events") && wire.at("events").is_array() && !wire.at("events").empty()) {
                    itemWire = wire.at("events").front().at("data").at("item").dump();
                }
                if (decoded) {
                    if (const auto* decodedBatch = std::get_if<frontend::EventBatch>(&decoded.value());
                        decodedBatch != nullptr && decodedBatch->events.size() == 1) {
                        decodedExact = decodedBatch->events.front().data.at("item").value("type", "") == stableType;
                    }
                }
            }
            const std::string stableWire = "\"type\":\"" + stableType + "\"";
            const std::string backendWire = "\"type\":\"" + std::string(mapping.backendType) + "\"";
            const bool noBackendDiscriminator = mapping.backendType == stableType || itemWire.find(backendWire) == std::string::npos;
            everyEventRoundTrips = everyEventRoundTrips && serialized.hasValue() && decodedExact &&
                                   itemWire.find(stableWire) != std::string::npos && noBackendDiscriminator;
            userWireNormalized = userWireNormalized ||
                                 (mapping.backendType == "user_message" && itemWire.find("\"type\":\"userMessage\"") != std::string::npos &&
                                  itemWire.find("\"type\":\"user_message\"") == std::string::npos);
            agentWireNormalized = agentWireNormalized || (mapping.backendType == "agent_message" &&
                                                          itemWire.find("\"type\":\"agentMessage\"") != std::string::npos &&
                                                          itemWire.find("\"type\":\"agent_message\"") == std::string::npos);
        }

        result.expectTrue(everyKindMapped && mappedKinds.size() == 18,
                          "the production backend item-kind mapper covers all 18 reviewed alternatives, including both tool-call forms");
        result.expectTrue(everyKindRoundTrips && mappedSpellings.size() == 18 && generatedSpellings == mappedSpellings,
                          "all 18 enum spellings are unique, generated-authority-defined, and round-trip through ThreadItemKind");
        result.expectTrue(everyItemSchemaValid && everyEventRoundTrips && everyIdentityExact,
                          "all 18 projected items satisfy ExpandedThreadItem and serialize/decode with exact parent identity");
        result.expectTrue(
            noRawOccurrenceRepresentation && userWireNormalized && agentWireNormalized && liveReplayEquivalent,
            "wire projection normalizes backend discriminators, copies no raw occurrence representation, and is live/replay stable");

        const BackendItemKindMapping userMapping{"user_message", frontend::ThreadItemKind::UserMessage};
        const BackendItemKindMapping agentMapping{"agent_message", frontend::ThreadItemKind::AgentMessage};
        backend::Snapshot exact = itemSnapshot(userMapping, "duplicate-item", "thread-exact", "turn-exact");
        backend::Snapshot unrelated = itemSnapshot(agentMapping, "duplicate-item", "thread-unrelated", "turn-unrelated");
        exact.threads.insert(exact.threads.begin(), std::move(unrelated.threads.front()));
        const detail::CanonicalEventRecord exactRecord = detail::makeCanonicalEventRecord(
            "item.updated",
            frontend::Json{
                {"threadId", "thread-exact"}, {"turnId", "turn-exact"}, {"item", {{"id", "duplicate-item"}, {"type", "user_message"}}}},
            exact,
            frontend::SequenceNumber{700});
        const detail::EventProjection exactProjection = detail::projectEvent(exactRecord, localExpandedContext());
        const detail::CanonicalEventRecord exactContentRecord =
            detail::makeCanonicalEventRecord("item.content.updated",
                                             frontend::Json{{"threadId", "thread-exact"},
                                                            {"turnId", "turn-exact"},
                                                            {"itemId", "duplicate-item"},
                                                            {"channel", "agentText"},
                                                            {"content", "exact replacement"}},
                                             exact,
                                             frontend::SequenceNumber{701});
        const detail::EventProjection exactContentProjection = detail::projectEvent(exactContentRecord, localExpandedContext());
        const detail::CanonicalEventRecord mismatchRecord = detail::makeCanonicalEventRecord(
            "item.updated",
            frontend::Json{
                {"threadId", "thread-exact"}, {"turnId", "turn-missing"}, {"item", {{"id", "duplicate-item"}, {"type", "user_message"}}}},
            exact,
            frontend::SequenceNumber{702});

        backend::Snapshot inconsistentParent = itemSnapshot(userMapping, "duplicate-item", "thread-exact", "turn-exact");
        inconsistentParent.threads.front().turns.front().threadId = "thread-inconsistent";
        const detail::CanonicalEventRecord inconsistentParentRecord = detail::makeCanonicalEventRecord(
            "item.updated",
            frontend::Json{
                {"threadId", "thread-exact"}, {"turnId", "turn-exact"}, {"item", {{"id", "duplicate-item"}, {"type", "user_message"}}}},
            inconsistentParent,
            frontend::SequenceNumber{703});
        result.expectTrue(
            !exactRecord.snapshotRequired && exactProjection.events.size() == 1 &&
                exactProjection.events.front().data.at("item").at("type") == "userMessage" &&
                exactProjection.events.front().data.at("item").at("threadId") == "thread-exact" &&
                exactContentProjection.events.size() == 1 && exactContentProjection.events.front().data.at("threadId") == "thread-exact" &&
                exactContentProjection.events.front().data.at("turnId") == "turn-exact" && mismatchRecord.snapshotRequired &&
                detail::projectEvent(mismatchRecord, localExpandedContext()).events.empty() && inconsistentParentRecord.snapshotRequired &&
                detail::projectEvent(inconsistentParentRecord, localExpandedContext()).events.empty(),
            "item upsert and content projection resolve the full thread/turn/item triple, reject inconsistent parent metadata, and "
            "never substitute a duplicate ID");
    }

    void testUnknownBackendItemContainment(tests::support::TestResult& result) {
        namespace backend = ai::openai::codex::backend;
        const BackendItemKindMapping knownMapping{"user_message", frontend::ThreadItemKind::UserMessage};
        backend::Snapshot snapshot = itemSnapshot(knownMapping, "known-item", "thread-unknown", "turn-unknown");
        backend::ItemSnapshot unknown;
        unknown.id = "future-item";
        unknown.type = "future_codex_item_kind";
        unknown.status = "completed";
        unknown.data = {{"safeLabel", "future item retained only in the backend"}};
        unknown.stamp = {23, backend::Freshness::Current};
        snapshot.threads.front().turns.front().items.push_back(std::move(unknown));

        const detail::CanonicalEventRecord event =
            detail::makeCanonicalEventRecord("item.updated",
                                             frontend::Json{{"threadId", "thread-unknown"},
                                                            {"turnId", "turn-unknown"},
                                                            {"item", {{"id", "future-item"}, {"type", "future_codex_item_kind"}}}},
                                             snapshot,
                                             frontend::SequenceNumber{710});
        const detail::EventProjection live = detail::projectEvent(event, localExpandedContext());
        const detail::EventProjection replay = detail::projectEvent(event, localExpandedContext(), frontend::SequenceNumber{709});

        const detail::CanonicalSnapshotRecord snapshotRecord = detail::makeCanonicalSnapshotRecord(
            frontend::Json{{"items", frontend::Json::array()}}, snapshot, frontend::SequenceNumber{711});
        const auto projectedSnapshot = detail::projectSnapshot(snapshotRecord, localExpandedContext());
        bool validBoundedSnapshot = false;
        if (projectedSnapshot) {
            const frontend::Json& state = projectedSnapshot->snapshot.state;
            const auto decoded = frontend::Codec::decodeExpandedSnapshot(expandedSnapshotEnvelope(*projectedSnapshot));
            validBoundedSnapshot = decoded.hasValue() && state.at("items").size() == 1 &&
                                   state.at("items").front().at("id") == "known-item" &&
                                   state.at("items").front().at("type") == "userMessage" && state.at("truncation").at("truncated") &&
                                   state.at("truncation").at("omittedEntries").get<std::size_t>() >= 1 &&
                                   state.dump().find("future_codex_item_kind") == std::string::npos &&
                                   state.dump().find("\"type\":\"unknown\"") == std::string::npos;
        }
        result.expectTrue(event.snapshotRequired && !detail::canonicalEventRetainedBytes(event).has_value() && live.events.empty() &&
                              replay.events.empty(),
                          "an unrecognized backend item emits no fabricated item event and requests authoritative Snapshot fallback");
        result.expectTrue(validBoundedSnapshot,
                          "expanded snapshots omit unsupported items through explicit truncation accounting while retaining known items");
    }

    void testAllPendingRequestRuntimeMappings(tests::support::TestResult& result) {
        namespace backend = ai::openai::codex::backend;
        backend::Snapshot snapshot;
        std::set<std::string> expectedKinds;
        for (std::size_t index = 0; index < generated::AllPendingRequestProjections.size(); ++index) {
            const generated::PendingRequestProjectionMetadata& metadata = generated::AllPendingRequestProjections[index];
            expectedKinds.emplace(metadata.kind);
            backend::PendingRequestSnapshot pending;
            pending.id = backend::PendingRequestId{static_cast<std::uint64_t>(index + 1)};
            pending.type = backendPendingType(metadata.kind);
            pending.threadId = "thread-pending";
            pending.turnId = "turn-pending";
            pending.itemId = "item-pending-" + std::to_string(index);
            pending.details = {{"summary", "safe request summary"}, {"safeOrdinal", index}, {"accessToken", SecretSentinel}};
            if (metadata.kind == "user_input") {
                pending.details["questions"] = frontend::Json::array(
                    {frontend::Json{{"id", "question-1"},
                                    {"header", "Header"},
                                    {"prompt", "Safe prompt"},
                                    {"allowsFreeText", true},
                                    {"secret", false},
                                    {"options", frontend::Json::array({frontend::Json{{"label", "One"}, {"description", "First"}}})}}});
                pending.details["autoResolutionMs"] = 60'000;
            }
            snapshot.pendingRequests.push_back(std::move(pending));
        }

        const detail::CanonicalSnapshotRecord record = detail::makeCanonicalSnapshotRecord(
            frontend::Json{{"pendingRequests", frontend::Json::array()}}, snapshot, frontend::SequenceNumber{220});
        const auto local = detail::projectSnapshot(record, localExpandedContext());
        const auto remote = detail::projectSnapshot(record, defaultExpandedContext());
        bool exactKindsAndLocations = local && remote && local->snapshot.state.at("pendingRequests").size() == 10 &&
                                      remote->snapshot.state.at("pendingRequests").size() == 10 &&
                                      stringsFromJsonArray(local->snapshot.state.at("pendingRequests"), "kind") == expectedKinds &&
                                      stringsFromJsonArray(remote->snapshot.state.at("pendingRequests"), "kind") == expectedKinds;
        bool safePresentations = exactKindsAndLocations;
        bool userInputPreserved = false;
        if (local) {
            const frontend::Json& pendingRequests = local->snapshot.state.at("pendingRequests");
            for (std::size_t index = 0; index < pendingRequests.size(); ++index) {
                const frontend::Json& pending = pendingRequests.at(index);
                safePresentations = safePresentations && pending.contains("threadId") && pending.at("threadId") == "thread-pending" &&
                                    pending.contains("turnId") && pending.at("turnId") == "turn-pending" && pending.contains("itemId") &&
                                    pending.contains("details") && pending.at("details").contains("safeOrdinal") &&
                                    !pending.at("details").contains("accessToken") &&
                                    pending.at("pendingRequestId") == std::to_string(index + 1) &&
                                    pending.at("kind") == generated::AllPendingRequestProjections[index].kind;
                if (pending.at("kind") == "user_input") {
                    userInputPreserved =
                        pending.contains("questions") && pending.contains("autoResolutionMs") && pending.at("questions").size() == 1 &&
                        pending.at("questions").at(0).at("id") == "question-1" && pending.at("questions").at(0).contains("isSecret") &&
                        !pending.at("questions").at(0).at("isSecret").get<bool>() &&
                        pending.at("questions").at(0).at("options").at(0).at("label") == "One" && pending.at("autoResolutionMs") == 60'000;
                }
            }
        }
        const bool schemaValid = local && remote && frontend::Codec::decodeExpandedSnapshot(expandedSnapshotEnvelope(*local)).hasValue() &&
                                 frontend::Codec::decodeExpandedSnapshot(expandedSnapshotEnvelope(*remote)).hasValue();
        result.expectTrue(
            exactKindsAndLocations && safePresentations && userInputPreserved && schemaValid &&
                !serializedContainsSecret(record.expandedState.value),
            "all ten pending-request kinds retain safe exact presentations, including bounded user-input questions, for Observe");

        const detail::CanonicalEventRecord event = detail::makeCanonicalEventRecord(
            "request.pending", frontend::Json{{"pendingRequestId", "1"}}, snapshot, frontend::SequenceNumber{230});
        const detail::EventProjection legacy = detail::projectEvent(event, localContext({}));
        const detail::EventProjection expanded =
            detail::projectEvent(event, localContext({frontend::FrontendCapability::DedicatedPendingRequests}));
        result.expectTrue(
            legacy.events.size() == 1 && legacy.events.front().type == "request.pending" && expanded.events.size() == 1 &&
                expanded.events.front().type == "pendingRequests.updated" &&
                legacy.events.front().sequence == expanded.events.front().sequence,
            "pending-request capability selection produces exactly one legacy or dedicated representation from one occurrence");
    }

    void testAllExpandedEventFamilies(tests::support::TestResult& result) {
        namespace backend = ai::openai::codex::backend;
        const backend::Snapshot snapshot = representativeBackendSnapshot();
        const std::set<std::string> expected{
            "provider.updated",    "controller.updated", "sessions.updated",      "threadList.updated",   "thread.upserted",
            "thread.removed",      "turn.upserted",      "item.upserted",         "item.content.updated", "pendingRequests.updated",
            "account.updated",     "models.updated",     "configuration.updated", "process.updated",      "filesystemWatch.updated",
            "fuzzySearch.updated", "reviews.updated",    "integrations.updated",  "plugins.updated",      "skills.updated",
            "mcp.updated",         "platform.updated",   "notice.added",          "activity.updated",     "capacity.updated",
            "diagnostics.updated",
        };
        std::set<std::string> observed;
        bool localValid = true;
        bool remoteScopeCorrect = true;
        bool exactSequence = true;
        for (std::size_t index = 0; index < detail::AllExpandedEventProjections.size(); ++index) {
            const frontend::ExpandedEventType type = detail::AllExpandedEventProjections[index].type;
            frontend::Json data{{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"itemId", "item-1"}};
            if (type == frontend::ExpandedEventType::ThreadUpserted) {
                data = {{"thread", {{"id", "thread-1"}}}};
            } else if (type == frontend::ExpandedEventType::ThreadRemoved) {
                data = {{"threadId", "thread-1"}};
            } else if (type == frontend::ExpandedEventType::TurnUpserted) {
                data = {{"turn", {{"id", "turn-1"}, {"threadId", "thread-1"}}}};
            } else if (type == frontend::ExpandedEventType::ItemUpserted) {
                data = {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"item", {{"id", "item-1"}}}};
            } else if (type == frontend::ExpandedEventType::ItemContentUpdated) {
                data = {{"threadId", "thread-1"},
                        {"turnId", "turn-1"},
                        {"itemId", "item-1"},
                        {"channel", "agentText"},
                        {"content", "exact replacement"}};
            } else if (type == frontend::ExpandedEventType::ProcessUpdated) {
                data = {{"process", {{"processHandle", "process-1"}}}};
            } else if (type == frontend::ExpandedEventType::FilesystemWatchUpdated) {
                data = {{"filesystemWatch", {{"watchId", "watch-1"}}}};
            } else if (type == frontend::ExpandedEventType::FuzzySearchUpdated) {
                data = {{"fuzzySearch", {{"sessionId", "search-1"}}}};
            } else if (type == frontend::ExpandedEventType::NoticeAdded) {
                data = {{"notice",
                         {{"category", "warning"}, {"summary", "exact notice"}, {"stamp", {{"generation", 9}, {"freshness", "current"}}}}}};
            } else if (type == frontend::ExpandedEventType::ActivityUpdated) {
                data = {{"activity",
                         {{"key", "activity-1"},
                          {"subjectId", "subject-1"},
                          {"kind", "test"},
                          {"lifecycle", "running"},
                          {"active", true},
                          {"stamp", {{"generation", 9}, {"freshness", "current"}}}}}};
            }
            const detail::CanonicalEventRecord record = detail::makeCanonicalEventRecord(
                std::string(frontend::toString(type)), std::move(data), snapshot, frontend::SequenceNumber{300 + index});
            const detail::EventProjection local = detail::projectEvent(record, localExpandedContext());
            const detail::EventProjection remote = detail::projectEvent(record, defaultExpandedContext());
            localValid = localValid && local.events.size() == 1 && local.events.front().type == frontend::toString(type);
            const bool remoteAuthorized = !detail::AllExpandedEventProjections[index].privilegedScope.has_value() ||
                                          defaultExpandedContext().hasScope(*detail::AllExpandedEventProjections[index].privilegedScope);
            remoteScopeCorrect = remoteScopeCorrect &&
                                 (remoteAuthorized ? remote.events.size() == 1 && remote.events.front().type == frontend::toString(type)
                                                   : remote.events.empty());
            if (!local.events.empty()) {
                observed.emplace(local.events.front().type);
                const auto decodedType = frontend::expandedEventTypeFromString(local.events.front().type);
                localValid = localValid && decodedType.has_value() &&
                             frontend::Codec::encodeExpandedEvent(frontend::ExpandedFrontendEvent{local.events.front().sequence,
                                                                                                  *decodedType,
                                                                                                  local.events.front().data,
                                                                                                  local.events.front().extensions})
                                 .hasValue();
                exactSequence = exactSequence && local.events.front().sequence.value() == 300 + index;
            }
        }
        result.expectTrue(localValid && remoteScopeCorrect && exactSequence && observed == expected && expected.size() == 26,
                          "all 26 expanded event families are schema-valid, sequence-stable, and obey full-family scope policy");
    }

    void testCapabilitySelectionAndUnknownFallback(tests::support::TestResult& result) {
        namespace backend = ai::openai::codex::backend;
        const backend::Snapshot snapshot = representativeBackendSnapshot();
        const generated::ProjectionMetadata& knownMetadata = generated::AllNotificationProjections.front();
        const detail::CanonicalEventRecord known =
            detail::makeCanonicalEventRecord("codex.extension",
                                             frontend::Json{{"method", notificationMethod(knownMetadata.registryKey)}, {"safe", true}},
                                             snapshot,
                                             frontend::SequenceNumber{400});
        const detail::EventProjection knownLegacy = detail::projectEvent(known, localContext({}));
        const detail::EventProjection knownUnrelated =
            detail::projectEvent(known, localContext({frontend::FrontendCapability::CompleteBackendDomains}));
        const detail::EventProjection knownExpanded =
            detail::projectEvent(known, localContext({frontend::FrontendCapability::DedicatedNotificationEvents}));

        const detail::CanonicalEventRecord unknown =
            detail::makeCanonicalEventRecord("codex.extension",
                                             frontend::Json{{"method", "future/unknownNotification"},
                                                            {"safe", "preserved"},
                                                            {"credential", SecretSentinel},
                                                            {"rawProviderEnvelope", frontend::Json{{"message", SecretSentinel}}}},
                                             snapshot,
                                             frontend::SequenceNumber{410});
        const detail::EventProjection unknownExpanded =
            detail::projectEvent(unknown, localContext({frontend::FrontendCapability::DedicatedNotificationEvents}));

        const bool knownExactlyOne = knownLegacy.events.size() == 1 && knownUnrelated.events.size() == 1 &&
                                     knownExpanded.events.size() == knownMetadata.expandedMappings.size() &&
                                     knownLegacy.events.front().type == "codex.extension" &&
                                     knownUnrelated.events.front().type == "codex.extension" &&
                                     std::none_of(knownExpanded.events.begin(), knownExpanded.events.end(), [](const auto& event) {
                                         return event.type == "codex.extension";
                                     });
        const bool unknownFallsBackSafely =
            !unknown.registryKey.has_value() && unknown.expandedEvents.empty() && unknownExpanded.events.size() == 1 &&
            unknownExpanded.events.front().type == "codex.extension" && unknownExpanded.events.front().data.contains("safe") &&
            unknownExpanded.events.front().data.at("safe") == "preserved" && !serializedContainsSecret(unknownExpanded.events.front().data);
        result.expectTrue(
            knownExactlyOne && unknownFallsBackSafely,
            "capabilities select exactly one known representation while unknown notifications retain one bounded redacted fallback");
    }

    void testCapabilityIndependentFieldFiltering(tests::support::TestResult& result) {
        namespace backend = ai::openai::codex::backend;
        const backend::Snapshot snapshot = representativeBackendSnapshot();
        const detail::CanonicalEventRecord record =
            detail::makeCanonicalEventRecord("item.content.updated",
                                             frontend::Json{{"threadId", "thread-1"},
                                                            {"turnId", "turn-1"},
                                                            {"itemId", "item-1"},
                                                            {"channel", "commandOutput"},
                                                            {"content", "PRIVILEGED_COMMAND_OUTPUT"}},
                                             snapshot,
                                             frontend::SequenceNumber{420});
        const detail::FrontendProjectionContext localLegacy = detail::makeProjectionContext(
            principal("local-legacy", frontend::LocalTrustedScopes, std::string(frontend::LocalTrustedScopeProfile.name)));
        const detail::FrontendProjectionContext remoteLegacy = detail::makeProjectionContext(
            principal("remote-legacy", frontend::DefaultRemoteScopes, std::string(frontend::DefaultRemoteScopeProfile.name)));
        const detail::EventProjection localLegacyProjection = detail::projectEvent(record, localLegacy);
        const detail::EventProjection remoteLegacyProjection = detail::projectEvent(record, remoteLegacy);
        const detail::EventProjection localExpanded =
            detail::projectEvent(record, localContext({frontend::FrontendCapability::CompleteThreadItems}));
        const detail::EventProjection remoteExpanded = detail::projectEvent(
            record,
            detail::makeProjectionContext(
                principal("remote-expanded", frontend::DefaultRemoteScopes, std::string(frontend::DefaultRemoteScopeProfile.name)),
                std::array{frontend::FrontendCapability::CompleteThreadItems}));
        const detail::EventProjection remoteExpandedWithMetadata = detail::projectEvent(
            record,
            detail::makeProjectionContext(
                principal("remote-expanded", frontend::DefaultRemoteScopes, std::string(frontend::DefaultRemoteScopeProfile.name)),
                std::array{frontend::FrontendCapability::CompleteThreadItems, frontend::FrontendCapability::ScopeProjectedState}));

        const bool localAuthorized =
            localLegacyProjection.events.size() == 1 && localExpanded.events.size() == 1 &&
            localLegacyProjection.events.front().data.contains("content") && localExpanded.events.front().data.contains("content") &&
            localLegacyProjection.events.front().data.at("content") == "PRIVILEGED_COMMAND_OUTPUT" &&
            localExpanded.events.front().data.at("content") == "PRIVILEGED_COMMAND_OUTPUT" &&
            frontend::Codec::encodeExpandedEvent(frontend::ExpandedFrontendEvent{localExpanded.events.front().sequence,
                                                                                 frontend::ExpandedEventType::ItemContentUpdated,
                                                                                 localExpanded.events.front().data,
                                                                                 localExpanded.events.front().extensions})
                .hasValue();
        const bool remoteAlwaysFiltered =
            remoteLegacyProjection.events.empty() && remoteExpanded.events.empty() && remoteExpandedWithMetadata.events.empty();
        result.expectTrue(localAuthorized && remoteAlwaysFiltered,
                          "a required-member content occurrence is schema-valid when authorized and wholly hidden below its scope ceiling");
    }

    void testSemanticItemOutputProjectionBeyondRuleBound(tests::support::TestResult& result) {
        namespace backend = ai::openai::codex::backend;
        constexpr std::size_t ItemCount = 200;

        backend::Snapshot snapshot;
        snapshot.provider.lifecycle = backend::ProviderLifecycle::Ready;
        snapshot.provider.generation = 17;
        snapshot.threadList.hasLoadedPage = true;
        snapshot.threadList.stamp = {17, backend::Freshness::Current};

        backend::TurnSnapshot turn;
        turn.id = "scope-turn";
        turn.threadId = "scope-thread";
        turn.status = "completed";
        turn.terminal = true;
        turn.stamp = {17, backend::Freshness::Current};
        frontend::Json legacyItems = frontend::Json::array();
        for (std::size_t index = 0; index < ItemCount; ++index) {
            const bool command = index % 2 == 0;
            const std::string id = "scope-item-" + std::to_string(index);
            const std::string type = command ? "commandExecution" : "fileChange";
            const std::string output = "scoped-output-" + std::to_string(index);

            backend::ItemSnapshot item;
            item.id = id;
            item.type = type;
            item.status = "completed";
            item.commandOutput = output;
            item.stamp = {17, backend::Freshness::Current};
            turn.items.push_back(std::move(item));
            legacyItems.push_back(frontend::Json{{"id", id},
                                                 {"threadId", "scope-thread"},
                                                 {"turnId", "scope-turn"},
                                                 {"type", type},
                                                 {"status", "completed"},
                                                 {"commandOutput", output}});
        }
        backend::ThreadSnapshot thread;
        thread.id = "scope-thread";
        thread.fullyLoaded = true;
        thread.stamp = {17, backend::Freshness::Current};
        thread.turns.push_back(std::move(turn));
        snapshot.threads.push_back(std::move(thread));

        const frontend::Json legacyState{
            {"threads",
             frontend::Json::array({frontend::Json{
                 {"id", "scope-thread"},
                 {"turns",
                  frontend::Json::array({frontend::Json{{"id", "scope-turn"}, {"threadId", "scope-thread"}, {"items", legacyItems}}})}}})},
            {"pendingRequests", frontend::Json::array()},
            {"codexExtensions", frontend::Json::array()},
        };
        const detail::CanonicalSnapshotRecord record =
            detail::makeCanonicalSnapshotRecord(legacyState, snapshot, frontend::SequenceNumber{421});

        const auto observeExpanded = detail::projectSnapshot(record, scopedContext({frontend::FrontendScope::Observe}, true));
        const auto commandExpanded = detail::projectSnapshot(
            record, scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::CommandExecution}, true));
        const auto filesystemExpanded = detail::projectSnapshot(
            record, scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::FilesystemWrite}, true));
        const auto bothExpanded = detail::projectSnapshot(
            record,
            scopedContext(
                {frontend::FrontendScope::Observe, frontend::FrontendScope::CommandExecution, frontend::FrontendScope::FilesystemWrite},
                true));
        const auto observeLegacy = detail::projectSnapshot(record, scopedContext({frontend::FrontendScope::Observe}, false));
        const auto commandLegacy = detail::projectSnapshot(
            record, scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::CommandExecution}, false));
        const auto filesystemLegacy = detail::projectSnapshot(
            record, scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::FilesystemWrite}, false));

        const auto outputsMatch = [](const frontend::Json& items, bool commandVisible, bool filesystemVisible) {
            if (!items.is_array() || items.size() != ItemCount) {
                return false;
            }
            for (std::size_t index = 0; index < items.size(); ++index) {
                const bool expected = index % 2 == 0 ? commandVisible : filesystemVisible;
                if (items[index].contains("commandOutput") != expected) {
                    return false;
                }
                if (expected && items[index].at("commandOutput") != "scoped-output-" + std::to_string(index)) {
                    return false;
                }
            }
            return true;
        };
        const auto legacyItemsFrom = [](const detail::SnapshotProjection& projection) -> const frontend::Json& {
            return projection.snapshot.state.at("threads").at(0).at("turns").at(0).at("items");
        };

        const bool exactAcrossBound =
            observeExpanded && commandExpanded && filesystemExpanded && bothExpanded && observeLegacy && commandLegacy &&
            filesystemLegacy && outputsMatch(observeExpanded->snapshot.state.at("items"), false, false) &&
            outputsMatch(commandExpanded->snapshot.state.at("items"), true, false) &&
            outputsMatch(filesystemExpanded->snapshot.state.at("items"), false, true) &&
            outputsMatch(bothExpanded->snapshot.state.at("items"), true, true) &&
            outputsMatch(legacyItemsFrom(*observeLegacy), false, false) && outputsMatch(legacyItemsFrom(*commandLegacy), true, false) &&
            outputsMatch(legacyItemsFrom(*filesystemLegacy), false, true) &&
            !observeExpanded->snapshot.state.at("items").at(ItemCount - 1).contains("commandOutput") &&
            filesystemExpanded->snapshot.state.at("items").at(ItemCount - 1).contains("commandOutput");
        result.expectTrue(
            exactAcrossBound && record.expandedState.rules.size() < detail::FrontendProjectionLimits{}.maximumProjectionRules &&
                record.legacyState.rules.size() < detail::FrontendProjectionLimits{}.maximumProjectionRules &&
                !record.sanitization.truncated,
            "type-specific commandOutput projection remains exact beyond 128 items without consuming the bounded rule budget");

        const frontend::Json& commandItem = legacyItems.at(198);
        const frontend::Json& fileItem = legacyItems.at(199);
        const detail::CanonicalEventRecord commandEvent =
            detail::makeCanonicalEventRecord("item.updated",
                                             frontend::Json{{"threadId", "scope-thread"}, {"turnId", "scope-turn"}, {"item", commandItem}},
                                             snapshot,
                                             frontend::SequenceNumber{422});
        const detail::CanonicalEventRecord fileEvent =
            detail::makeCanonicalEventRecord("item.updated",
                                             frontend::Json{{"threadId", "scope-thread"}, {"turnId", "scope-turn"}, {"item", fileItem}},
                                             snapshot,
                                             frontend::SequenceNumber{423});
        const detail::EventProjection commandVisible = detail::projectEvent(
            commandEvent, scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::CommandExecution}, true));
        const detail::EventProjection commandHidden = detail::projectEvent(
            commandEvent, scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::FilesystemWrite}, true));
        const detail::EventProjection fileVisible = detail::projectEvent(
            fileEvent, scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::FilesystemWrite}, true));
        const detail::EventProjection fileHidden = detail::projectEvent(
            fileEvent, scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::CommandExecution}, true));
        const detail::EventProjection fileReplay =
            detail::projectEvent(fileEvent,
                                 scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::FilesystemWrite}, true),
                                 frontend::SequenceNumber{422});
        const detail::EventProjection commandVisibleLegacy = detail::projectEvent(
            commandEvent, scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::CommandExecution}, false));
        const detail::EventProjection commandHiddenLegacy = detail::projectEvent(
            commandEvent, scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::FilesystemWrite}, false));
        const detail::EventProjection fileVisibleLegacy = detail::projectEvent(
            fileEvent, scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::FilesystemWrite}, false));
        const detail::EventProjection fileHiddenLegacy = detail::projectEvent(
            fileEvent, scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::CommandExecution}, false));
        result.expectTrue(
            commandVisible.events.size() == 1 && commandHidden.events.size() == 1 && fileVisible.events.size() == 1 &&
                fileHidden.events.size() == 1 && commandVisibleLegacy.events.size() == 1 && commandHiddenLegacy.events.size() == 1 &&
                fileVisibleLegacy.events.size() == 1 && fileHiddenLegacy.events.size() == 1 &&
                commandVisible.events.front().data.at("item").contains("commandOutput") &&
                !commandHidden.events.front().data.at("item").contains("commandOutput") &&
                fileVisible.events.front().data.at("item").contains("commandOutput") &&
                !fileHidden.events.front().data.at("item").contains("commandOutput") &&
                commandVisibleLegacy.events.front().data.at("item").contains("commandOutput") &&
                !commandHiddenLegacy.events.front().data.at("item").contains("commandOutput") &&
                fileVisibleLegacy.events.front().data.at("item").contains("commandOutput") &&
                !fileHiddenLegacy.events.front().data.at("item").contains("commandOutput") && fileReplay.events == fileVisible.events,
            "legacy item.updated and expanded item.upserted apply the discriminator-specific ceiling identically in live "
            "and replay projection");

        const detail::CanonicalEventRecord fileContentEvent =
            detail::makeCanonicalEventRecord("item.content.updated",
                                             frontend::Json{{"threadId", "scope-thread"},
                                                            {"turnId", "scope-turn"},
                                                            {"itemId", "scope-item-199"},
                                                            {"channel", "commandOutput"},
                                                            {"content", "scoped-output-199"}},
                                             snapshot,
                                             frontend::SequenceNumber{425});
        const detail::EventProjection fileContentCommand = detail::projectEvent(
            fileContentEvent, scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::CommandExecution}, true));
        const detail::EventProjection fileContentFilesystem = detail::projectEvent(
            fileContentEvent, scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::FilesystemWrite}, true));
        const detail::EventProjection fileContentCommandLegacy = detail::projectEvent(
            fileContentEvent, scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::CommandExecution}, false));
        const detail::EventProjection fileContentFilesystemLegacy = detail::projectEvent(
            fileContentEvent, scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::FilesystemWrite}, false));
        result.expectTrue(fileContentCommand.events.empty() && fileContentFilesystem.events.size() == 1 &&
                              fileContentCommandLegacy.events.empty() && fileContentFilesystemLegacy.events.size() == 1 &&
                              fileContentFilesystem.events.front().data.at("content") == "scoped-output-199" &&
                              fileContentFilesystemLegacy.events.front().data.at("content") == "scoped-output-199",
                          "a normalized file-change commandOutput occurrence uses the exact item type for whole-event scope suppression "
                          "in expanded and legacy representations");

        const auto semanticItemEvent = [&snapshot](std::string id, frontend::Json item, std::uint64_t sequence) {
            item["id"] = id;
            return detail::makeCanonicalEventRecord(
                "item.updated",
                frontend::Json{{"threadId", "scope-thread"}, {"turnId", "scope-turn"}, {"item", std::move(item)}},
                snapshot,
                frontend::SequenceNumber{sequence});
        };
        const detail::CanonicalEventRecord unknownItem = semanticItemEvent(
            "scope-item-unknown", frontend::Json{{"type", "futureOutputItem"}, {"commandOutput", "unknown-ceiling-output"}}, 426);
        const detail::CanonicalEventRecord inconsistentItem = semanticItemEvent(
            "scope-item-inconsistent",
            frontend::Json{
                {"type", "commandExecution"}, {"commandOutput", "inconsistent-ceiling-output"}, {"data", {{"codexType", "fileChange"}}}},
            427);
        bool conservativeUnknownAndInconsistent = true;
        const std::array unsafeItems{
            frontend::Json{{"type", "futureOutputItem"}, {"commandOutput", "unknown-ceiling-output"}},
            frontend::Json{
                {"type", "commandExecution"}, {"commandOutput", "inconsistent-ceiling-output"}, {"data", {{"codexType", "fileChange"}}}},
        };
        for (std::size_t index = 0; index < unsafeItems.size(); ++index) {
            const std::vector<frontend::FrontendScope> scopes = detail::itemCommandOutputRequiredScopes(unsafeItems[index]);
            const detail::CanonicalEventRecord& record = index == 0 ? unknownItem : inconsistentItem;
            conservativeUnknownAndInconsistent =
                conservativeUnknownAndInconsistent && scopes.size() == 2 &&
                std::find(scopes.begin(), scopes.end(), frontend::FrontendScope::CommandExecution) != scopes.end() &&
                std::find(scopes.begin(), scopes.end(), frontend::FrontendScope::FilesystemWrite) != scopes.end() &&
                record.snapshotRequired && detail::projectEvent(record, localExpandedContext()).events.empty();
        }
        result.expectTrue(conservativeUnknownAndInconsistent,
                          "unknown or inconsistent item discriminators retain conservative information ceilings and emit no raw item");

        const detail::CanonicalEventRecord nestedThreadEvent = detail::makeCanonicalEventRecord(
            "thread.updated", frontend::Json{{"thread", legacyState.at("threads").at(0)}}, snapshot, frontend::SequenceNumber{424});
        const detail::EventProjection nestedObserve =
            detail::projectEvent(nestedThreadEvent, scopedContext({frontend::FrontendScope::Observe}, false));
        const detail::EventProjection nestedFilesystem = detail::projectEvent(
            nestedThreadEvent, scopedContext({frontend::FrontendScope::Observe, frontend::FrontendScope::FilesystemWrite}, false));
        const auto nestedItems = [](const detail::EventProjection& projection) -> const frontend::Json& {
            return projection.events.front().data.at("thread").at("turns").at(0).at("items");
        };
        result.expectTrue(nestedObserve.events.size() == 1 && nestedFilesystem.events.size() == 1 &&
                              outputsMatch(nestedItems(nestedObserve), false, false) &&
                              outputsMatch(nestedItems(nestedFilesystem), false, true),
                          "a legacy nested thread event projects all 200 item outputs semantically without per-item rules or tail leaks");
    }

    void testLegacyNestedProjectionParity(tests::support::TestResult& result) {
        const auto snapshot = representativeBackendSnapshot();
        const frontend::Json legacy = representativeLegacyState();
        const frontend::Json& thread = legacy.at("threads").at(0);
        frontend::Json turn = thread.at("turns").at(0);
        turn["threadId"] = "thread-1";
        const frontend::Json& item = turn.at("items").at(0);
        const detail::FrontendProjectionContext local = detail::makeProjectionContext(
            principal("local-legacy", frontend::LocalTrustedScopes, std::string(frontend::LocalTrustedScopeProfile.name)));
        const detail::FrontendProjectionContext remote = detail::makeProjectionContext(
            principal("remote-legacy", frontend::DefaultRemoteScopes, std::string(frontend::DefaultRemoteScopeProfile.name)));
        const detail::FrontendProjectionContext remoteExpanded = detail::makeProjectionContext(
            principal("remote-expanded", frontend::DefaultRemoteScopes, std::string(frontend::DefaultRemoteScopeProfile.name)),
            std::array{frontend::FrontendCapability::CompleteThreadItems, frontend::FrontendCapability::DedicatedNotificationEvents});

        const std::array records{
            detail::makeCanonicalEventRecord("thread.updated", frontend::Json{{"thread", thread}}, snapshot, frontend::SequenceNumber{430}),
            detail::makeCanonicalEventRecord("turn.updated", frontend::Json{{"turn", turn}}, snapshot, frontend::SequenceNumber{431}),
            detail::makeCanonicalEventRecord("item.updated",
                                             frontend::Json{{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"item", item}},
                                             snapshot,
                                             frontend::SequenceNumber{432}),
        };

        bool localRetains = true;
        bool remoteFiltersLiveReplayAndExpanded = true;
        for (const auto& record : records) {
            const detail::EventProjection localLive = detail::projectEvent(record, local);
            const detail::EventProjection remoteLive = detail::projectEvent(record, remote);
            const detail::EventProjection remoteReplay =
                detail::projectEvent(record, remote, frontend::SequenceNumber{record.sequence.value() - 1});
            const detail::EventProjection expandedLive = detail::projectEvent(record, remoteExpanded);
            localRetains =
                localRetains && !localLive.events.empty() && localLive.events.front().data.dump().find("PRIVILEGED_") != std::string::npos;
            remoteFiltersLiveReplayAndExpanded =
                remoteFiltersLiveReplayAndExpanded && !remoteLive.events.empty() && remoteLive.events == remoteReplay.events &&
                remoteLive.events.front().data.dump().find("PRIVILEGED_") == std::string::npos && !expandedLive.events.empty() &&
                std::none_of(expandedLive.events.begin(), expandedLive.events.end(), [](const frontend::FrontendEvent& event) {
                    return event.data.dump().find("PRIVILEGED_") != std::string::npos;
                });
        }
        result.expectTrue(localRetains && remoteFiltersLiveReplayAndExpanded,
                          "legacy snapshot/live/replay filtering covers nested thread, turn, item, command, filesystem, and extension data "
                          "before representation selection");
    }

    void testExactExpandedEntityIdentity(tests::support::TestResult& result) {
        namespace backend = ai::openai::codex::backend;
        backend::Snapshot snapshot = representativeBackendSnapshot();

        backend::ItemSnapshot fileItem;
        fileItem.id = "file-item-1";
        fileItem.type = "fileChange";
        fileItem.status = "completed";
        fileItem.commandOutput = "bounded privileged output";
        fileItem.stamp = {9, backend::Freshness::Current};
        snapshot.threads.front().turns.front().items.push_back(std::move(fileItem));

        backend::ItemSnapshot otherItem;
        otherItem.id = "item-last";
        otherItem.type = "agentMessage";
        otherItem.status = "completed";
        otherItem.agentText = "last item text";
        otherItem.stamp = {9, backend::Freshness::Current};
        backend::TurnSnapshot otherTurn;
        otherTurn.id = "turn-last";
        otherTurn.threadId = "thread-last";
        otherTurn.status = "unrelated-last-status";
        otherTurn.terminal = true;
        otherTurn.stamp = {9, backend::Freshness::Current};
        otherTurn.items.push_back(std::move(otherItem));
        backend::ThreadSnapshot otherThread;
        otherThread.id = "thread-last";
        otherThread.title = "Unrelated last thread";
        otherThread.preview = "unrelated last preview";
        otherThread.fullyLoaded = true;
        otherThread.stamp = {9, backend::Freshness::Current};
        otherThread.turns.push_back(std::move(otherTurn));
        snapshot.threads.push_back(std::move(otherThread));
        snapshot.processes.push_back(
            {"process-last", "running", 0, 0, false, false, 0, std::nullopt, {9, backend::Freshness::Current}, false});
        snapshot.filesystemWatches.push_back({"watch-last", std::nullopt, 7, {9, backend::Freshness::Current}, false});
        snapshot.fuzzySearchSessions.push_back({"search-last", 9, true, {9, backend::Freshness::Current}, false});

        const auto projected = [&snapshot](std::string type, frontend::Json data, std::uint64_t sequence) {
            const detail::CanonicalEventRecord record =
                detail::makeCanonicalEventRecord(std::move(type), std::move(data), snapshot, frontend::SequenceNumber{sequence});
            return std::pair{record, detail::projectEvent(record, localExpandedContext(), frontend::SequenceNumber{sequence - 1})};
        };

        const auto [threadRecord, threadProjection] = projected("thread.updated", frontend::Json{{"thread", {{"id", "thread-1"}}}}, 500);
        const auto [turnRecord, turnProjection] =
            projected("turn.updated", frontend::Json{{"turn", {{"id", "turn-1"}, {"threadId", "thread-1"}}}}, 501);
        const detail::CanonicalEventRecord turnStartedRecord = detail::makeCanonicalEventRecord(
            "codex.extension",
            frontend::Json{{"method", "turn/started"}, {"params", {{"threadId", "thread-1"}, {"turn", {{"id", "turn-1"}}}}}},
            snapshot,
            frontend::SequenceNumber{520});
        const detail::EventProjection turnStartedProjection = detail::projectEvent(turnStartedRecord, localExpandedContext());
        const auto [itemRecord, itemProjection] =
            projected("item.updated", frontend::Json{{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"item", {{"id", "item-1"}}}}, 502);
        const auto [contentRecord, contentProjection] = projected("item.content.updated",
                                                                  frontend::Json{{"threadId", "thread-1"},
                                                                                 {"turnId", "turn-1"},
                                                                                 {"itemId", "item-1"},
                                                                                 {"channel", "agentText"},
                                                                                 {"content", "exact replacement"}},
                                                                  503);
        const detail::CanonicalEventRecord fileOutputRecord =
            detail::makeCanonicalEventRecord("codex.extension",
                                             frontend::Json{{"method", "item/fileChange/outputDelta"},
                                                            {"params",
                                                             {{"threadId", "thread-1"},
                                                              {"turnId", "turn-1"},
                                                              {"itemId", "file-item-1"},
                                                              {"delta", "provider delta is not the accumulated value"}}}},
                                             snapshot,
                                             frontend::SequenceNumber{504});
        const detail::EventProjection fileOutputProjection = detail::projectEvent(fileOutputRecord, localExpandedContext());
        detail::FrontendProjectionContext commandOnly = localExpandedContext();
        commandOnly.scopes = {frontend::FrontendScope::Observe, frontend::FrontendScope::CommandExecution};
        detail::FrontendProjectionContext filesystemWrite = localExpandedContext();
        filesystemWrite.scopes = {frontend::FrontendScope::Observe, frontend::FrontendScope::FilesystemWrite};
        const detail::EventProjection fileOutputCommandOnly = detail::projectEvent(fileOutputRecord, commandOnly);
        const detail::EventProjection fileOutputFilesystemWrite = detail::projectEvent(fileOutputRecord, filesystemWrite);
        const detail::CanonicalEventRecord mcpProgressRecord = detail::makeCanonicalEventRecord(
            "codex.extension",
            frontend::Json{
                {"method", "item/mcpToolCall/progress"},
                {"params", {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"itemId", "item-1"}, {"message", "bounded progress"}}}},
            snapshot,
            frontend::SequenceNumber{505});
        const detail::EventProjection mcpProgressProjection = detail::projectEvent(mcpProgressRecord, localExpandedContext());
        const auto [processRecord, processProjection] =
            projected("process.updated", frontend::Json{{"process", {{"processHandle", "process-1"}}}}, 506);
        const auto [watchRecord, watchProjection] =
            projected("filesystemWatch.updated", frontend::Json{{"filesystemWatch", {{"watchId", "watch-1"}}}}, 507);
        const auto [searchRecord, searchProjection] =
            projected("fuzzySearch.updated", frontend::Json{{"fuzzySearch", {{"sessionId", "search-1"}}}}, 508);
        const auto [noticeRecord, noticeProjection] =
            projected("notice.added",
                      frontend::Json{{"notice",
                                      {{"category", "warning"},
                                       {"summary", "triggering notice, not the retained tail"},
                                       {"stamp", {{"generation", 9}, {"freshness", "current"}}}}}},
                      509);
        const auto [activityRecord, activityProjection] =
            projected("activity.updated",
                      frontend::Json{{"activity",
                                      {{"key", "activity-trigger"},
                                       {"subjectId", "subject-trigger"},
                                       {"kind", "test"},
                                       {"lifecycle", "completed"},
                                       {"active", false},
                                       {"stamp", {{"generation", 9}, {"freshness", "current"}}}}}},
                      510);
        const detail::CanonicalEventRecord deleted =
            detail::makeCanonicalEventRecord("codex.extension",
                                             frontend::Json{{"method", "thread/deleted"}, {"params", {{"threadId", "thread-1"}}}},
                                             snapshot,
                                             frontend::SequenceNumber{511});
        const detail::EventProjection deletedProjection = detail::projectEvent(deleted, localExpandedContext());

        const auto providerRecord = [&snapshot](std::string method, frontend::Json params, std::uint64_t sequence) {
            return detail::makeCanonicalEventRecord("codex.extension",
                                                    frontend::Json{{"method", std::move(method)}, {"params", std::move(params)}},
                                                    snapshot,
                                                    frontend::SequenceNumber{sequence});
        };
        const std::array turnProviderRecords{
            providerRecord("thread/compacted", {{"threadId", "thread-1"}, {"turnId", "turn-1"}}, 521),
            providerRecord("thread/tokenUsage/updated",
                           {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"tokenUsage", frontend::Json::object()}},
                           522),
            providerRecord("model/rerouted",
                           {{"threadId", "thread-1"},
                            {"turnId", "turn-1"},
                            {"fromModel", "model-before"},
                            {"toModel", "model-after"},
                            {"reason", "test"}},
                           523),
            providerRecord("model/safetyBuffering/updated",
                           {{"threadId", "thread-1"},
                            {"turnId", "turn-1"},
                            {"model", "model-after"},
                            {"reasons", frontend::Json::array()},
                            {"showBufferingUi", false},
                            {"useCases", frontend::Json::array()}},
                           524),
            providerRecord(
                "model/verification", {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"verifications", frontend::Json::array()}}, 525),
        };
        bool exactTurnProviderMappings = true;
        for (const detail::CanonicalEventRecord& record : turnProviderRecords) {
            const detail::EventProjection projection = detail::projectEvent(record, localExpandedContext());
            const detail::EventProjection replay =
                detail::projectEvent(record, localExpandedContext(), frontend::SequenceNumber{record.sequence.value() - 1});
            exactTurnProviderMappings =
                exactTurnProviderMappings && projection.events.size() == 1 && projection.events.front().type == "turn.upserted" &&
                projection.events.front().data.at("turn").at("id") == "turn-1" &&
                projection.events.front().data.at("turn").at("threadId") == "thread-1" &&
                projection.events.front().data.at("turn").at("status") == "completed" && projection.events == replay.events;
        }
        const detail::CanonicalEventRecord threadProviderRecord = providerRecord("thread/archived", {{"threadId", "thread-1"}}, 526);
        const detail::EventProjection threadProviderProjection = detail::projectEvent(threadProviderRecord, localExpandedContext());
        const detail::CanonicalEventRecord itemProviderRecord = providerRecord(
            "item/started", {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"item", {{"id", "item-1"}}}, {"startedAtMs", 1}}, 527);
        const detail::EventProjection itemProviderProjection = detail::projectEvent(itemProviderRecord, localExpandedContext());
        const detail::CanonicalEventRecord processProviderRecord =
            providerRecord("process/outputDelta", {{"processHandle", "process-1"}, {"deltaBase64", ""}, {"stream", "stdout"}}, 528);
        const detail::EventProjection processProviderProjection = detail::projectEvent(processProviderRecord, localExpandedContext());
        const detail::CanonicalEventRecord watchProviderRecord =
            providerRecord("fs/changed", {{"watchId", "watch-1"}, {"changedPaths", frontend::Json::array()}}, 529);
        const detail::EventProjection watchProviderProjection = detail::projectEvent(watchProviderRecord, localExpandedContext());
        const detail::CanonicalEventRecord searchProviderRecord =
            providerRecord("fuzzyFileSearch/sessionCompleted", {{"sessionId", "search-1"}}, 530);
        const detail::EventProjection searchProviderProjection = detail::projectEvent(searchProviderRecord, localExpandedContext());
        const detail::CanonicalEventRecord noticeProviderRecord =
            providerRecord("warning", {{"message", "exact provider notice occurrence"}}, 531);
        const detail::EventProjection noticeProviderProjection = detail::projectEvent(noticeProviderRecord, localExpandedContext());

        result.expectTrue(threadProjection.events.size() == 1 && threadProjection.events.front().data.at("thread").at("id") == "thread-1" &&
                              threadProjection.events.front().data.at("thread").at("title") == "Projection thread",
                          "thread projection selects the exact canonical target instead of the last retained thread");
        result.expectTrue(turnProjection.events.size() == 1 && turnProjection.events.front().data.at("turn").at("id") == "turn-1" &&
                              turnProjection.events.front().data.at("turn").at("threadId") == "thread-1",
                          "turn projection selects the exact canonical target and parent instead of the last retained turn");
        result.expectTrue(turnStartedProjection.events.size() == 1 && turnStartedProjection.events.front().type == "turn.upserted" &&
                              turnStartedProjection.events.front().data.at("turn").at("id") == "turn-1" &&
                              turnStartedProjection.events.front().data.at("turn").at("threadId") == "thread-1",
                          "turn/started resolves the reviewed params.turn.id plus params.threadId provider shape exactly");
        result.expectTrue(itemProjection.events.size() == 1 && itemProjection.events.front().data.at("item").at("id") == "item-1" &&
                              itemProjection.events.front().data.at("item").at("turnId") == "turn-1" &&
                              itemProjection.events.front().data.at("item").at("threadId") == "thread-1",
                          "item projection selects the exact canonical target and parents instead of the last retained item");
        result.expectTrue(contentProjection.events.size() == 1 && contentProjection.events.front().data.at("itemId") == "item-1" &&
                              contentProjection.events.front().data.at("turnId") == "turn-1" &&
                              contentProjection.events.front().data.at("threadId") == "thread-1" &&
                              contentProjection.events.front().data.at("content") == "exact replacement",
                          "item-content projection preserves the exact canonical target, parents, and replacement content");
        result.expectTrue(fileOutputProjection.events.size() == 1 && fileOutputProjection.events.front().type == "item.content.updated" &&
                              fileOutputProjection.events.front().data.at("itemId") == "file-item-1" &&
                              fileOutputProjection.events.front().data.at("channel") == "commandOutput" &&
                              fileOutputProjection.events.front().data.at("content") == "bounded privileged output" &&
                              fileOutputCommandOnly.events.empty() && fileOutputFilesystemWrite.events.size() == 1 &&
                              fileOutputFilesystemWrite.events.front().data.at("content") == "bounded privileged output",
                          "file-change output projects the exact accumulated item channel under the filesystem-write ceiling and hides "
                          "the complete required-member event below that ceiling");
        result.expectTrue(mcpProgressProjection.events.size() == 1 && mcpProgressProjection.events.front().type == "item.upserted" &&
                              mcpProgressProjection.events.front().data.at("item").at("id") == "item-1",
                          "non-content item notifications refresh the exact item instead of fabricating a content replacement");
        result.expectTrue(processProjection.events.size() == 1 &&
                              processProjection.events.front().data.at("process").at("processHandle") == "process-1",
                          "process projection selects the exact canonical process instead of the last retained process");
        result.expectTrue(watchProjection.events.size() == 1 &&
                              watchProjection.events.front().data.at("filesystemWatch").at("watchId") == "watch-1",
                          "filesystem-watch projection selects the exact canonical watch instead of the last retained watch");
        result.expectTrue(searchProjection.events.size() == 1 &&
                              searchProjection.events.front().data.at("fuzzySearch").at("sessionId") == "search-1",
                          "fuzzy-search projection selects the exact canonical search instead of the last retained search");
        result.expectTrue(noticeProjection.events.size() == 1 &&
                              noticeProjection.events.front().data.at("notice").at("summary") == "triggering notice, not the retained tail",
                          "notice projection preserves the exact canonical occurrence instead of the last retained notice");
        result.expectTrue(activityProjection.events.size() == 1 &&
                              activityProjection.events.front().data.at("activity").at("key") == "activity-trigger",
                          "activity projection preserves the exact canonical occurrence instead of the last retained activity");
        result.expectTrue(deletedProjection.events.size() == 1 && deletedProjection.events.front().type == "thread.removed" &&
                              deletedProjection.events.front().data.at("threadId") == "thread-1",
                          "thread/deleted projects the exact identity through the thread.removed family");
        result.expectTrue(exactTurnProviderMappings,
                          "turn-mutating thread and model notifications project the exact non-last turn identity, parent, and content");
        result.expectTrue(threadProviderProjection.events.size() == 1 &&
                              threadProviderProjection.events.front().data.at("thread").at("id") == "thread-1" &&
                              threadProviderProjection.events.front().data.at("thread").at("title") == "Projection thread" &&
                              itemProviderProjection.events.size() == 1 &&
                              itemProviderProjection.events.front().data.at("item").at("id") == "item-1" &&
                              itemProviderProjection.events.front().data.at("item").at("status") == "completed",
                          "provider thread and item wrappers resolve exact identities and matching retained content");
        result.expectTrue(processProviderProjection.events.size() == 1 &&
                              processProviderProjection.events.front().data.at("process").at("processHandle") == "process-1" &&
                              processProviderProjection.events.front().data.at("process").at("stdoutBytes") == 24 &&
                              watchProviderProjection.events.size() == 1 &&
                              watchProviderProjection.events.front().data.at("filesystemWatch").at("watchId") == "watch-1" &&
                              watchProviderProjection.events.front().data.at("filesystemWatch").at("root") == "/private/worktree" &&
                              searchProviderProjection.events.size() == 1 &&
                              searchProviderProjection.events.front().data.at("fuzzySearch").at("sessionId") == "search-1" &&
                              searchProviderProjection.events.front().data.at("fuzzySearch").at("resultCount") == 2,
                          "provider process, watch, and search wrappers select exact non-last entities and matching content");
        result.expectTrue(noticeProviderProjection.events.size() == 1 &&
                              noticeProviderProjection.events.front().data.at("notice").at("summary") == "exact provider notice occurrence",
                          "provider notice projection carries the triggering occurrence rather than a retained snapshot tail");

        bool liveReplayEqual = true;
        for (const detail::CanonicalEventRecord* record : {&threadRecord,
                                                           &turnRecord,
                                                           &turnStartedRecord,
                                                           &itemRecord,
                                                           &contentRecord,
                                                           &fileOutputRecord,
                                                           &mcpProgressRecord,
                                                           &processRecord,
                                                           &watchRecord,
                                                           &searchRecord,
                                                           &noticeRecord,
                                                           &activityRecord,
                                                           &threadProviderRecord,
                                                           &itemProviderRecord,
                                                           &processProviderRecord,
                                                           &watchProviderRecord,
                                                           &searchProviderRecord,
                                                           &noticeProviderRecord}) {
            const detail::EventProjection live = detail::projectEvent(*record, localExpandedContext());
            const detail::EventProjection replay =
                detail::projectEvent(*record, localExpandedContext(), frontend::SequenceNumber{record->sequence.value() - 1});
            liveReplayEqual = liveReplayEqual && live.events == replay.events;
        }
        result.expectTrue(liveReplayEqual, "exact identity and content are byte-identical in live and replay projection");

        const detail::CanonicalEventRecord missing = detail::makeCanonicalEventRecord(
            "thread.updated", frontend::Json{{"thread", frontend::Json::object()}}, snapshot, frontend::SequenceNumber{512});
        result.expectTrue(missing.snapshotRequired && !detail::canonicalEventRetainedBytes(missing).has_value() &&
                              detail::projectEvent(missing, localExpandedContext()).events.empty(),
                          "an unprovable identity requests snapshot fallback instead of fabricating or substituting an entity");

        const std::array conflictingRecords{
            detail::makeCanonicalEventRecord(
                "thread.updated", frontend::Json{{"turn", {{"threadId", "thread-1"}}}}, snapshot, frontend::SequenceNumber{513}),
            detail::makeCanonicalEventRecord(
                "thread.removed", frontend::Json{{"thread", {{"id", "thread-1"}}}}, snapshot, frontend::SequenceNumber{514}),
            detail::makeCanonicalEventRecord("turn.updated",
                                             frontend::Json{{"turn", {{"id", "turn-1"}}}, {"threadId", "thread-1"}},
                                             snapshot,
                                             frontend::SequenceNumber{515}),
            detail::makeCanonicalEventRecord(
                "item.updated",
                frontend::Json{{"item", {{"id", "item-1"}}}, {"params", {{"threadId", "thread-1"}, {"turnId", "turn-1"}}}},
                snapshot,
                frontend::SequenceNumber{516}),
        };
        result.expectTrue(std::all_of(conflictingRecords.begin(),
                                      conflictingRecords.end(),
                                      [](const auto& record) {
                                          return record.snapshotRequired && !detail::canonicalEventRetainedBytes(record).has_value();
                                      }),
                          "identity-bearing families reject cross-family or mixed-wrapper paths instead of guessing a target");

        const std::array conflictingProviderRecords{
            detail::makeCanonicalEventRecord(
                "codex.extension",
                frontend::Json{{"method", "thread/started"}, {"params", {{"thread", {{"id", "thread-1"}}}, {"threadId", "thread-last"}}}},
                snapshot,
                frontend::SequenceNumber{532}),
            detail::makeCanonicalEventRecord(
                "codex.extension",
                frontend::Json{{"method", "turn/started"},
                               {"params", {{"threadId", "thread-1"}, {"turn", {{"id", "turn-1"}, {"threadId", "thread-last"}}}}}},
                snapshot,
                frontend::SequenceNumber{533}),
            detail::makeCanonicalEventRecord(
                "codex.extension",
                frontend::Json{
                    {"method", "item/started"},
                    {"params", {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"itemId", "item-last"}, {"item", {{"id", "item-1"}}}}}},
                snapshot,
                frontend::SequenceNumber{534}),
            detail::makeCanonicalEventRecord("codex.extension",
                                             frontend::Json{{"method", "process/outputDelta"},
                                                            {"params", {{"processHandle", "process-1"}}},
                                                            {"process", {{"processHandle", "process-last"}}}},
                                             snapshot,
                                             frontend::SequenceNumber{535}),
        };
        result.expectTrue(std::all_of(conflictingProviderRecords.begin(),
                                      conflictingProviderRecords.end(),
                                      [](const auto& record) {
                                          return record.snapshotRequired && !detail::canonicalEventRetainedBytes(record).has_value() &&
                                                 detail::projectEvent(record, localExpandedContext()).events.empty();
                                      }),
                          "conflicting recognized provider identity paths fail closed to deterministic snapshot fallback");

        backend::Snapshot redactedCollisionSnapshot = snapshot;
        redactedCollisionSnapshot.processes.push_back(
            {"[redacted]", "running", 3, 0, false, false, 0, std::nullopt, {9, backend::Freshness::Current}, false});
        redactedCollisionSnapshot.filesystemWatches.push_back(
            {"[redacted]", std::optional<std::string>{"/must-not-project"}, 3, {9, backend::Freshness::Current}, false});
        redactedCollisionSnapshot.fuzzySearchSessions.push_back({"[redacted]", 3, false, {9, backend::Freshness::Current}, false});
        const std::array redactedIdentityRecords{
            detail::makeCanonicalEventRecord("codex.extension",
                                             frontend::Json{{"method", "command/exec/outputDelta"},
                                                            {"params", {{"processId", "[redacted]"}}},
                                                            {"sensitiveFieldsRedacted", true}},
                                             redactedCollisionSnapshot,
                                             frontend::SequenceNumber{536}),
            detail::makeCanonicalEventRecord(
                "codex.extension",
                frontend::Json{{"method", "fs/changed"}, {"params", {{"watchId", "[redacted]"}}}, {"sensitiveFieldsRedacted", true}},
                redactedCollisionSnapshot,
                frontend::SequenceNumber{537}),
            detail::makeCanonicalEventRecord("codex.extension",
                                             frontend::Json{{"method", "fuzzyFileSearch/sessionUpdated"},
                                                            {"params", {{"sessionId", "[redacted]"}}},
                                                            {"sensitiveFieldsRedacted", true}},
                                             redactedCollisionSnapshot,
                                             frontend::SequenceNumber{538}),
            detail::makeCanonicalEventRecord("codex.extension",
                                             frontend::Json{{"method", "process/outputDelta"},
                                                            {"params", {{"processHandle", "process-1"}}},
                                                            {"truncation", {{"params", frontend::Json::object()}}}},
                                             redactedCollisionSnapshot,
                                             frontend::SequenceNumber{539}),
        };
        const detail::CanonicalEventRecord unrelatedRedactionRecord =
            detail::makeCanonicalEventRecord("codex.extension",
                                             frontend::Json{{"method", "thread/archived"},
                                                            {"params", {{"threadId", "thread-1"}, {"cwd", "[redacted]"}}},
                                                            {"sensitiveFieldsRedacted", true}},
                                             redactedCollisionSnapshot,
                                             frontend::SequenceNumber{540});
        const detail::EventProjection unrelatedRedactionProjection = detail::projectEvent(unrelatedRedactionRecord, localExpandedContext());
        const detail::CanonicalEventRecord literalIdentityRecord = detail::makeCanonicalEventRecord(
            "codex.extension",
            frontend::Json{{"method", "command/exec/outputDelta"}, {"params", {{"processId", "[redacted]"}}}},
            redactedCollisionSnapshot,
            frontend::SequenceNumber{541});
        const detail::EventProjection literalIdentityProjection = detail::projectEvent(literalIdentityRecord, localExpandedContext());
        result.expectTrue(std::all_of(redactedIdentityRecords.begin(),
                                      redactedIdentityRecords.end(),
                                      [](const auto& record) {
                                          return record.snapshotRequired && !detail::canonicalEventRetainedBytes(record).has_value() &&
                                                 detail::projectEvent(record, localExpandedContext()).events.empty();
                                      }),
                          "redacted or truncated provider identity cannot collide with a literal retained sentinel entity");
        result.expectTrue(unrelatedRedactionProjection.events.size() == 1 &&
                              unrelatedRedactionProjection.events.front().data.at("thread").at("id") == "thread-1" &&
                              literalIdentityProjection.events.size() == 1 &&
                              literalIdentityProjection.events.front().data.at("process").at("processHandle") == "[redacted]",
                          "unrelated field redaction preserves a proven identity while an unflagged literal sentinel remains legitimate");

        const detail::CanonicalEventRecord missingTarget = detail::makeCanonicalEventRecord(
            "thread.updated", frontend::Json{{"thread", {{"id", "thread-missing"}}}}, snapshot, frontend::SequenceNumber{517});
        result.expectTrue(missingTarget.snapshotRequired && !detail::canonicalEventRetainedBytes(missingTarget).has_value(),
                          "a proven identity missing from the captured snapshot requests snapshot fallback without substitution");

        backend::Snapshot boundedIdentitySnapshot = snapshot;
        backend::ThreadSnapshot prefixThread;
        prefixThread.id = std::string(1'024, 'x');
        prefixThread.title = "bounded prefix must not collide";
        boundedIdentitySnapshot.threads.push_back(std::move(prefixThread));
        const detail::CanonicalEventRecord oversizedIdentity =
            detail::makeCanonicalEventRecord("thread.updated",
                                             frontend::Json{{"thread", {{"id", std::string(1'025, 'x')}}}},
                                             boundedIdentitySnapshot,
                                             frontend::SequenceNumber{518});
        std::string invalidUtf8Identity{"thread-invalid-"};
        invalidUtf8Identity.push_back(static_cast<char>(0xc0));
        invalidUtf8Identity.push_back(static_cast<char>(0xaf));
        const detail::CanonicalEventRecord invalidIdentity =
            detail::makeCanonicalEventRecord("thread.updated",
                                             frontend::Json{{"thread", {{"id", std::move(invalidUtf8Identity)}}}},
                                             boundedIdentitySnapshot,
                                             frontend::SequenceNumber{519});
        result.expectTrue(oversizedIdentity.snapshotRequired && invalidIdentity.snapshotRequired,
                          "oversized or invalid-UTF-8 identities fail exact resolution instead of truncating into another entity");

        backend::Snapshot pageSnapshot;
        std::set<std::string> expectedPageIds;
        std::set<std::string> projectedPageIds;
        bool exactPageContent = true;
        for (std::size_t index = 0; index < 35; ++index) {
            backend::ThreadSnapshot pageThread;
            pageThread.id = "page-thread-" + std::to_string(index);
            pageThread.title = "page title " + std::to_string(index);
            pageThread.preview = "page preview " + std::to_string(index);
            pageThread.stamp = {9, backend::Freshness::Current};
            pageSnapshot.threads.push_back(std::move(pageThread));
            if (index < 25) {
                expectedPageIds.emplace("page-thread-" + std::to_string(index));
            }
        }
        for (std::size_t index = 0; index < 25; ++index) {
            const std::string id = "page-thread-" + std::to_string(index);
            const detail::CanonicalEventRecord record = detail::makeCanonicalEventRecord(
                "thread.updated", frontend::Json{{"thread", {{"id", id}}}}, pageSnapshot, frontend::SequenceNumber{600 + index});
            const detail::EventProjection event = detail::projectEvent(record, localExpandedContext());
            if (event.events.size() != 1) {
                exactPageContent = false;
                continue;
            }
            const frontend::Json& thread = event.events.front().data.at("thread");
            projectedPageIds.emplace(thread.at("id").get<std::string>());
            exactPageContent = exactPageContent && thread.at("title") == "page title " + std::to_string(index) &&
                               thread.at("preview") == "page preview " + std::to_string(index);
        }
        result.expectTrue(projectedPageIds == expectedPageIds && projectedPageIds.size() == 25 && exactPageContent,
                          "a 25-thread page projects the exact 25 unique IDs and matching per-ID content over 35 retained threads");
    }

    void testCompleteProjectionMetadata(tests::support::TestResult& result) {
        const bool notificationLookups = std::all_of(
            generated::AllNotificationProjections.begin(),
            generated::AllNotificationProjections.end(),
            [](const generated::ProjectionMetadata& metadata) {
                const auto* found = detail::notificationProjection(metadata.registryKey);
                return found == &metadata && !metadata.expandedMappings.empty() &&
                       std::all_of(metadata.expandedMappings.begin(), metadata.expandedMappings.end(), [](std::string_view mapping) {
                           return frontend::expandedEventTypeFromString(mapping).has_value();
                       });
            });
        const bool itemLookups = std::all_of(generated::AllThreadItemProjections.begin(),
                                             generated::AllThreadItemProjections.end(),
                                             [](const generated::ProjectionMetadata& metadata) {
                                                 return detail::threadItemProjection(metadata.registryKey) == &metadata &&
                                                        metadata.expandedMappings.size() == 1 &&
                                                        metadata.expandedMappings.front() == "item.upserted";
                                             });
        result.expectTrue(detail::projectionMetadataIsComplete() && notificationLookups && itemLookups &&
                              generated::AllNotificationProjections.size() == 68 && generated::AllThreadItemProjections.size() == 18,
                          "the private runtime seam consumes all 68 generated notification and all 18 ThreadItem mappings exactly");

        const bool pendingKinds =
            std::all_of(generated::AllPendingRequestProjections.begin(),
                        generated::AllPendingRequestProjections.end(),
                        [](const generated::PendingRequestProjectionMetadata& metadata) {
                            const auto kind = frontend::pendingRequestKindFromString(metadata.kind);
                            return kind.has_value() && detail::pendingRequestProjection(*kind) == &metadata &&
                                   metadata.expansionCapability == generated::Capability::DedicatedPendingRequests &&
                                   metadata.exposure == "DedicatedPendingRequestContract" &&
                                   metadata.securityDecision == "ScopeProjectedStateEventApproved" &&
                                   metadata.redactionClass == "safe_pending_request" &&
                                   metadata.expandedEvent == "pendingRequests.updated" && metadata.presentationRequiredScopes.size() == 1 &&
                                   metadata.presentationRequiredScopes.front() == frontend::FrontendScope::Observe &&
                                   !metadata.controllerRequiredForPresentation && metadata.responseRequiredScopes.size() == 2 &&
                                   std::find(metadata.responseRequiredScopes.begin(),
                                             metadata.responseRequiredScopes.end(),
                                             frontend::FrontendScope::Control) != metadata.responseRequiredScopes.end() &&
                                   std::find(metadata.responseRequiredScopes.begin(),
                                             metadata.responseRequiredScopes.end(),
                                             frontend::FrontendScope::SensitiveResponse) != metadata.responseRequiredScopes.end() &&
                                   metadata.controllerRequiredForResponse &&
                                   metadata.duplicateSuppression == "exactly_one_compatibility_representation_per_connection";
                        });
        const bool eventKinds =
            std::all_of(detail::AllExpandedEventProjections.begin(),
                        detail::AllExpandedEventProjections.end(),
                        [](const detail::ExpandedEventProjectionMetadata& metadata) {
                            return frontend::expandedEventTypeFromString(frontend::toString(metadata.type)) == metadata.type;
                        });
        result.expectTrue(pendingKinds && eventKinds && generated::AllPendingRequestProjections.size() == 10 &&
                              detail::AllExpandedEventProjections.size() == 26,
                          "the runtime seam consumes the generated ten pending-request contracts and covers 26 expanded event families");

        const auto normalizedNotifications = std::count_if(generated::AllNotificationProjections.begin(),
                                                           generated::AllNotificationProjections.end(),
                                                           [](const generated::ProjectionMetadata& metadata) {
                                                               return metadata.legacyContract == "legacy_normalized";
                                                           });
        const auto normalizedItems = std::count_if(generated::AllThreadItemProjections.begin(),
                                                   generated::AllThreadItemProjections.end(),
                                                   [](const generated::ProjectionMetadata& metadata) {
                                                       return metadata.legacyContract == "legacy_normalized";
                                                   });
        result.expectTrue(normalizedNotifications == 14 && normalizedItems == 8,
                          "projection metadata retains the frozen 14/54 notification and 8/10 ThreadItem compatibility splits");
    }
} // namespace

int main() {
    tests::support::TestResult result;

    static_assert(generated::AllNotificationProjections.size() == 68);
    static_assert(generated::AllThreadItemProjections.size() == 18);
    static_assert(generated::AllPendingRequestProjections.size() == 10);
    static_assert(detail::AllExpandedEventProjections.size() == 26);

    testKnownStructuredSecretRemovalAndBounds(result);
    testKnownStructuredSecretsAndPotentiallySensitiveText(result);
    testSnapshotProjection(result);
    testProjectionResourceBounds(result);
    testIndependentSnapshotCapabilities(result);
    testEventProjectionAndReuse(result);
    testBackendProjectionBuilderAndGeneratedMappings(result);
    testAllThreadItemRuntimeMappings(result);
    testBackendItemKindNormalizationAndWireBoundary(result);
    testUnknownBackendItemContainment(result);
    testAllPendingRequestRuntimeMappings(result);
    testAllExpandedEventFamilies(result);
    testCapabilitySelectionAndUnknownFallback(result);
    testCapabilityIndependentFieldFiltering(result);
    testSemanticItemOutputProjectionBeyondRuleBound(result);
    testLegacyNestedProjectionParity(result);
    testExactExpandedEntityIdentity(result);
    testCompleteProjectionMetadata(result);

    return result.processResult();
}
