/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_BACKEND_SNAPSHOT_H
#define AI_OPENAI_CODEX_BACKEND_SNAPSHOT_H

#include "ai/openai/codex/backend/BackendState.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ai::openai::codex::backend {

    // Public snapshots deliberately use stricter, transport-friendly bounds
    // than the canonical reducer state. A normalized codex.extension event
    // built from one ExtensionSnapshot therefore fits the default frontend
    // batch bound even for adversarial string escaping.
    inline constexpr std::size_t MaxSnapshotCodexExtensions = 64;
    inline constexpr std::size_t MaxSnapshotExtensionMethodBytes = 1024;
    inline constexpr std::size_t MaxSnapshotExtensionPayloadBytes = 32U * 1024U;
    inline constexpr std::size_t MaxSnapshotJsonBytes = 64U * 1024U;
    inline constexpr std::size_t MaxSnapshotCommandOutputBytes = 4U * 1024U * 1024U;
    inline constexpr std::size_t MaxSnapshotExtensionDecodingErrorBytes = 2U * 1024U;
    inline constexpr std::size_t MaxSerializedCodexExtensionEventBytes = 64U * 1024U;
    inline constexpr std::size_t MaxSerializedCodexExtensionEnvelopeOverheadBytes = 4U * 1024U;
    static_assert(MaxSnapshotExtensionMethodBytes * 6U + MaxSnapshotExtensionPayloadBytes + MaxSnapshotExtensionDecodingErrorBytes * 6U +
                          MaxSerializedCodexExtensionEnvelopeOverheadBytes <=
                      MaxSerializedCodexExtensionEventBytes,
                  "escaped extension fields plus the normalized envelope must remain within the documented event bound");

    struct ErrorSnapshot {
        std::string category;
        std::int64_t code = 0;
        std::string message;

        bool operator==(const ErrorSnapshot&) const = default;
    };

    struct UserMessageSnapshot {
        std::optional<std::string> clientId;
        std::string text;
        bool textTruncated = false;
        bool contentTruncated = false;
        std::uint64_t originalContentBytes = 0;
        std::uint64_t retainedContentBytes = 0;
        std::uint64_t originalContentItems = 0;
        std::uint64_t retainedContentItems = 0;
        std::vector<std::string> textParts;

        bool operator==(const UserMessageSnapshot&) const = default;
    };

    struct ItemSnapshot {
        std::string id;
        std::string type;
        std::string status;
        std::string agentText;
        std::string reasoningText;
        std::string reasoningSummary;
        std::string commandOutput;
        std::uint64_t agentTextDroppedContentBytes = 0;
        std::uint64_t reasoningTextDroppedContentBytes = 0;
        std::uint64_t reasoningSummaryDroppedContentBytes = 0;
        std::uint64_t commandOutputDroppedContentBytes = 0;
        std::uint64_t droppedContentBytes = 0;
        bool contentTruncated = false;
        std::optional<std::int64_t> startedAtMs;
        std::optional<std::int64_t> completedAtMs;
        std::optional<UserMessageSnapshot> userMessage;
        Json data = Json::object();
        Json extensions = Json::object();
        SourceStamp stamp;
        bool connectionInvalidated = false;

        bool operator==(const ItemSnapshot&) const = default;
    };

    struct ItemSnapshotKey {
        typed::ThreadId threadId;
        typed::TurnId turnId;
        typed::ItemId itemId;

        bool operator==(const ItemSnapshotKey&) const = default;
    };

    struct ItemSnapshotBatch {
        SequenceNumber sequence;
        std::vector<ItemSnapshot> items;

        bool operator==(const ItemSnapshotBatch&) const = default;
    };

    enum class ItemContentSnapshotChannel { AgentText, ReasoningText, ReasoningSummary, CommandOutput };

    struct ItemContentSnapshotKey {
        typed::ThreadId threadId;
        typed::TurnId turnId;
        typed::ItemId itemId;
        ItemContentSnapshotChannel channel = ItemContentSnapshotChannel::AgentText;

        bool operator==(const ItemContentSnapshotKey&) const = default;
    };

    // Exact streaming projection needs one bounded channel and only the small
    // amount of item metadata required to validate it.  It deliberately does
    // not retain the other content channels or general item details.
    struct ItemContentSnapshot {
        ItemContentSnapshotKey key;
        std::string type;
        std::string status;
        std::string content;
        // Item-wide projected truncation accumulated before the selected
        // channel's final frontend character bound is applied.
        std::uint64_t droppedContentBytes = 0;
        // Canonical backend retention is tracked separately so append and
        // unchanged-prefix decisions do not confuse projection truncation
        // with backend rolling retention.
        std::uint64_t backendDroppedContentBytes = 0;
        // Bit positions follow ItemContentSnapshotChannel and identify
        // unselected channels that the frontend character bound omits.
        std::uint8_t frontendOmittedContentChannels = 0;
        bool contentTruncated = false;
        bool knownType = false;
        bool connectionInvalidated = false;

        bool operator==(const ItemContentSnapshot&) const = default;
    };

    struct ItemContentSnapshotBatch {
        SequenceNumber sequence;
        std::vector<ItemContentSnapshot> items;

        bool operator==(const ItemContentSnapshotBatch&) const = default;
    };

    struct TurnSnapshot {
        std::string id;
        std::string threadId;
        std::string status;
        bool active = false;
        bool terminal = false;
        std::optional<Json> failure;
        std::optional<Json> tokenUsage;
        std::optional<TurnPlanState> plan;
        std::optional<Json> effectiveExecutionConfiguration;
        std::optional<std::string> effectiveExecutionConfigurationProvenance;
        std::vector<ItemSnapshot> items;
        Json extensions = Json::object();
        SourceStamp stamp;
        bool connectionInvalidated = false;

        bool operator==(const TurnSnapshot&) const = default;
    };

    struct RealtimeThreadSnapshot {
        std::string lifecycle;
        std::string transcript;
        std::optional<std::string> lastError;
        std::optional<std::string> sessionId;
        std::optional<std::string> version;
        std::optional<std::size_t> lastSdpBytes;
        std::size_t itemCount = 0;
        std::uint64_t receivedAudioBytes = 0;
        std::uint64_t droppedAudioBytes = 0;
        bool transcriptTruncated = false;
        SourceStamp stamp;

        bool operator==(const RealtimeThreadSnapshot&) const = default;
    };

    struct ThreadSnapshot {
        std::string id;
        std::optional<std::string> title;
        std::optional<std::string> cwd;
        std::optional<std::string> model;
        std::optional<std::string> modelProvider;
        std::optional<std::string> preview;
        std::optional<std::string> status;
        std::optional<bool> ephemeral;
        std::optional<bool> archived;
        std::optional<Json> executionConfiguration;
        std::optional<std::int64_t> createdAt;
        std::optional<std::int64_t> updatedAt;
        bool fullyLoaded = false;
        std::vector<TurnSnapshot> turns;
        Json extensions = Json::object();
        RealtimeThreadSnapshot realtime;
        SourceStamp stamp;

        bool operator==(const ThreadSnapshot&) const = default;
    };

    struct ThreadSnapshotAtSequence {
        SequenceNumber sequence;
        std::optional<ThreadSnapshot> thread;

        bool operator==(const ThreadSnapshotAtSequence&) const = default;
    };

    struct PendingRequestSnapshot {
        PendingRequestId id;
        std::string type;
        std::optional<std::string> threadId;
        std::optional<std::string> turnId;
        std::optional<std::string> itemId;
        Json details = Json::object();

        bool operator==(const PendingRequestSnapshot&) const = default;
    };

    struct SessionSnapshot {
        SessionId id;
        SessionRole role = SessionRole::Observer;

        bool operator==(const SessionSnapshot&) const = default;
    };

    struct ThreadListSnapshot {
        bool hasLoadedPage = false;
        bool complete = false;
        std::optional<std::string> nextCursor;
        std::optional<std::string> backwardsCursor;
        std::size_t pagesLoaded = 0;
        SourceStamp stamp;

        bool operator==(const ThreadListSnapshot&) const = default;
    };

    struct ExtensionSnapshot {
        std::string method;
        Json payload = nullptr;
        std::optional<std::string> decodingError;
        bool methodTruncated = false;
        bool payloadTruncated = false;
        bool decodingErrorTruncated = false;
        bool sensitiveFieldsRedacted = false;
        std::uint64_t originalMethodBytes = 0;
        std::optional<std::uint64_t> originalPayloadBytes;
        std::uint64_t originalDecodingErrorBytes = 0;

        bool operator==(const ExtensionSnapshot&) const = default;
    };

    struct InitializeResponseSnapshot {
        std::string codexHome;
        std::string platformFamily;
        std::string platformOs;
        std::string userAgent;

        bool operator==(const InitializeResponseSnapshot&) const = default;
    };

    struct ProviderSnapshot {
        ProviderLifecycle lifecycle = ProviderLifecycle::Stopped;
        std::uint64_t generation = 0;
        bool desiredRunning = false;
        std::optional<ErrorSnapshot> lastError;
        RecoveryState recovery;
        std::optional<InitializeResponseSnapshot> initialization;

        bool operator==(const ProviderSnapshot&) const = default;
    };

    struct CapacitySnapshot {
        CapacityState state;
        std::size_t retainedThreads = 0;
        std::size_t retainedTurns = 0;
        std::size_t retainedItems = 0;
        std::size_t accumulatedContentBytes = 0;
        std::size_t retainedNotices = 0;
        std::size_t retainedProcesses = 0;
        std::size_t accumulatedProcessOutputBytes = 0;
        std::size_t retainedFilesystemWatches = 0;
        std::size_t retainedFuzzySearchSessions = 0;
        std::size_t retainedActivityRecords = 0;
        std::size_t omittedThreads = 0;
        std::size_t omittedTurns = 0;
        std::size_t omittedItems = 0;
        std::size_t sourceSessionCount = 0;
        std::size_t sourcePendingRequestCount = 0;
        bool truncated = false;
        bool mandatoryCoreExceedsLimit = false;

        bool operator==(const CapacitySnapshot&) const = default;
    };

    struct ProviderOperationSnapshot {
        std::string method;
        std::size_t resultAlternative = 0;
        SourceStamp stamp;

        bool operator==(const ProviderOperationSnapshot&) const = default;
    };

    struct ProviderNotificationSnapshot {
        std::string method;
        std::size_t eventAlternative = 0;
        SourceStamp stamp;

        bool operator==(const ProviderNotificationSnapshot&) const = default;
    };

    struct ProviderResultSummarySnapshot {
        std::string method;
        std::size_t resultAlternative = 0;
        std::string status;
        std::optional<std::string> subjectId;
        std::optional<std::string> nextCursor;
        std::size_t itemCount = 0;
        bool complete = true;
        SourceStamp stamp;

        bool operator==(const ProviderResultSummarySnapshot&) const = default;
    };

    struct ProviderDomainSnapshot {
        std::vector<std::string> latestNotificationMethods;
        std::vector<ProviderNotificationSnapshot> latestNotifications;
        std::vector<ProviderResultSummarySnapshot> latestResults;

        bool operator==(const ProviderDomainSnapshot&) const = default;
    };

    struct AccountDomainSnapshot : ProviderDomainSnapshot {
        std::optional<AccountLoginFlowState> login;
        std::optional<AccountAuthenticationState> authentication;
        std::optional<AccountRateLimitState> rateLimits;
        std::optional<std::string> resetCreditOutcome;
        SourceStamp resetCreditStamp;
        bool loggedOut = false;
        SourceStamp logoutStamp;

        bool operator==(const AccountDomainSnapshot&) const = default;
    };

    struct IntegrationsDomainSnapshot : ProviderDomainSnapshot {
        std::optional<AppCatalogState> apps;

        struct MarketplaceMutation {
            std::string operation;
            std::optional<std::string> marketplaceName;
            std::optional<std::string> installedRoot;
            std::size_t selectedCount = 0;
            std::size_t upgradedRootCount = 0;
            std::size_t errorCount = 0;
            bool alreadyAdded = false;
            bool truncated = false;
            SourceStamp stamp;

            bool operator==(const MarketplaceMutation&) const = default;
        };

        std::optional<MarketplaceMutation> marketplaceAdd;
        std::optional<MarketplaceMutation> marketplaceRemove;
        std::optional<MarketplaceMutation> marketplaceUpgrade;

        bool operator==(const IntegrationsDomainSnapshot&) const = default;
    };

    struct ConfigurationDomainSnapshot : ProviderDomainSnapshot {
        struct Write {
            std::string filePath;
            std::string status;
            std::string version;
            bool overridden = false;
            bool truncated = false;
            SourceStamp stamp;

            bool operator==(const Write&) const = default;
        };

        struct FeatureEnablement {
            std::vector<std::pair<std::string, bool>> entries;
            std::size_t totalEntries = 0;
            bool truncated = false;
            SourceStamp stamp;

            bool operator==(const FeatureEnablement&) const = default;
        };

        std::optional<Write> lastWrite;
        std::optional<FeatureEnablement> experimentalFeatureEnablement;

        bool operator==(const ConfigurationDomainSnapshot&) const = default;
    };

    struct ConversationDomainSnapshot : ProviderDomainSnapshot {
        struct GoalMutation {
            std::string operation;
            std::string threadId;
            std::optional<std::string> objective;
            std::optional<std::string> status;
            std::optional<bool> cleared;
            SourceStamp stamp;

            bool operator==(const GoalMutation&) const = default;
        };

        std::optional<GoalMutation> latestGoal;
        std::optional<GoalMutation> latestGoalClear;
        std::optional<GoalMutation> latestGoalSet;
        std::optional<GoalMutation> latestUnsubscribe;

        bool operator==(const ConversationDomainSnapshot&) const = default;
    };

    struct PluginsAndSkillsDomainSnapshot : ProviderDomainSnapshot {
        struct Mutation {
            std::string operation;
            std::optional<std::string> subjectId;
            std::optional<std::string> status;
            std::size_t itemCount = 0;
            bool truncated = false;
            SourceStamp stamp;

            bool operator==(const Mutation&) const = default;
        };

        std::optional<Mutation> pluginInstall;
        std::optional<Mutation> pluginShareCheckout;
        std::optional<Mutation> pluginShareSave;
        std::optional<Mutation> pluginShareUpdateTargets;
        std::optional<Mutation> skillsConfigWrite;
        std::optional<SkillsExtraRootsState> extraRoots;

        bool operator==(const PluginsAndSkillsDomainSnapshot&) const = default;
    };

    struct McpDomainSnapshot : ProviderDomainSnapshot {
        std::optional<McpOauthState> oauth;
        std::optional<McpStartupState> startup;
        std::optional<McpStatusListState> statusList;

        bool operator==(const McpDomainSnapshot&) const = default;
    };

    struct PlatformDomainSnapshot : ProviderDomainSnapshot {
        std::optional<RemoteControlState> remoteControl;
        std::optional<WindowsSandboxState> windowsSandbox;

        bool operator==(const PlatformDomainSnapshot&) const = default;
    };

    struct NoticeSnapshot {
        std::uint64_t occurrence = 0;
        NoticeCategory category = NoticeCategory::Warning;
        std::string summary;
        std::optional<std::string> details;
        std::optional<std::string> threadId;
        SourceStamp stamp;

        bool operator==(const NoticeSnapshot&) const = default;
    };

    struct ProcessSnapshot {
        std::string processHandle;
        std::string lifecycle;
        std::size_t stdoutBytes = 0;
        std::size_t stderrBytes = 0;
        bool stdoutTruncated = false;
        bool stderrTruncated = false;
        std::uint64_t droppedOutputBytes = 0;
        std::optional<std::int32_t> exitCode;
        SourceStamp stamp;
        bool connectionInvalidated = false;

        bool operator==(const ProcessSnapshot&) const = default;
    };

    struct FilesystemWatchSnapshot {
        std::string watchId;
        std::optional<std::string> root;
        std::size_t changedPathCount = 0;
        SourceStamp stamp;
        bool connectionInvalidated = false;

        bool operator==(const FilesystemWatchSnapshot&) const = default;
    };

    struct FuzzySearchSnapshot {
        std::string sessionId;
        std::size_t resultCount = 0;
        bool complete = false;
        SourceStamp stamp;
        bool connectionInvalidated = false;

        bool operator==(const FuzzySearchSnapshot&) const = default;
    };

    struct ActivitySnapshot {
        std::string key;
        std::string subjectId;
        std::string kind;
        std::string lifecycle;
        std::optional<std::string> summary;
        std::optional<std::string> details;
        std::optional<std::string> threadId;
        std::optional<std::string> turnId;
        SourceStamp stamp;
        bool active = false;

        bool operator==(const ActivitySnapshot&) const = default;
    };

    struct Snapshot {
        SequenceNumber sequence;
        ProviderSnapshot provider;
        CapacitySnapshot capacity;
        DiagnosticSummary diagnostics;
        std::vector<ThreadSnapshot> threads;
        std::vector<PendingRequestSnapshot> pendingRequests;
        std::optional<SessionId> controller;
        std::vector<SessionSnapshot> sessions;
        ThreadListSnapshot threadList;
        std::vector<ProviderOperationSnapshot> providerOperations;
        AccountDomainSnapshot accounts;
        ProviderDomainSnapshot models;
        ConfigurationDomainSnapshot configuration;
        ConversationDomainSnapshot conversations;
        ProviderDomainSnapshot filesystem;
        ProviderDomainSnapshot reviews;
        IntegrationsDomainSnapshot integrations;
        PluginsAndSkillsDomainSnapshot pluginsAndSkills;
        McpDomainSnapshot mcp;
        PlatformDomainSnapshot platform;
        std::vector<NoticeSnapshot> notices;
        std::vector<ProcessSnapshot> processes;
        std::vector<FilesystemWatchSnapshot> filesystemWatches;
        std::vector<FuzzySearchSnapshot> fuzzySearchSessions;
        std::vector<ActivitySnapshot> activities;
        std::vector<ExtensionSnapshot> recentExtensions;
        std::size_t omittedRecentExtensions = 0;
        bool sequenceExhausted = false;

        bool operator==(const Snapshot&) const;
        bool operator!=(const Snapshot&) const;
    };

    Snapshot makeSnapshot(const BackendState& state);
    [[nodiscard]] std::optional<ThreadSnapshot> makeThreadSnapshot(const BackendState& state, const typed::ThreadId& id);

    namespace detail {
        struct ThreadSnapshotEncodingInstrumentation {
            std::size_t jsonConstructions = 0;
            std::size_t dumpCalls = 0;

            bool operator==(const ThreadSnapshotEncodingInstrumentation&) const = default;
        };

        void resetThreadSnapshotEncodingInstrumentation() noexcept;
        [[nodiscard]] ThreadSnapshotEncodingInstrumentation threadSnapshotEncodingInstrumentation() noexcept;
    } // namespace detail

    std::size_t threadSnapshotSizeBytes(const ThreadSnapshot& snapshot) noexcept;
    [[nodiscard]] std::optional<ItemSnapshotBatch> makeItemSnapshotBatch(const BackendState& state,
                                                                         std::span<const ItemSnapshotKey> keys);
    [[nodiscard]] std::optional<ItemContentSnapshotBatch>
    makeItemContentSnapshotBatch(const BackendState& state, std::span<const ItemContentSnapshotKey> keys);
    std::size_t snapshotSizeBytes(const Snapshot& snapshot) noexcept;
    ExtensionSnapshot makeExtensionSnapshot(const ExtensionRecord& extension);

} // namespace ai::openai::codex::backend

#endif // AI_OPENAI_CODEX_BACKEND_SNAPSHOT_H
