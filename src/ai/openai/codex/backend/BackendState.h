/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_BACKEND_BACKENDSTATE_H
#define AI_OPENAI_CODEX_BACKEND_BACKENDSTATE_H

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/typed/Accounts.h"
#include "ai/openai/codex/typed/Apps.h"
#include "ai/openai/codex/typed/Commands.h"
#include "ai/openai/codex/typed/Configuration.h"
#include "ai/openai/codex/typed/Conversation.h"
#include "ai/openai/codex/typed/Events.h"
#include "ai/openai/codex/typed/ExternalAgents.h"
#include "ai/openai/codex/typed/Feedback.h"
#include "ai/openai/codex/typed/Filesystem.h"
#include "ai/openai/codex/typed/Hooks.h"
#include "ai/openai/codex/typed/Items.h"
#include "ai/openai/codex/typed/Marketplace.h"
#include "ai/openai/codex/typed/Mcp.h"
#include "ai/openai/codex/typed/Models.h"
#include "ai/openai/codex/typed/PermissionProfiles.h"
#include "ai/openai/codex/typed/Plugins.h"
#include "ai/openai/codex/typed/Reviews.h"
#include "ai/openai/codex/typed/ServerRequests.h"
#include "ai/openai/codex/typed/Skills.h"
#include "ai/openai/codex/typed/Threads.h"
#include "ai/openai/codex/typed/Turns.h"
#include "ai/openai/codex/typed/WindowsSandbox.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace ai::openai::codex::backend {

    class SessionId {
    public:
        constexpr SessionId() noexcept = default;
        explicit constexpr SessionId(std::uint64_t value) noexcept
            : id(value) {
        }

        constexpr std::uint64_t value() const noexcept {
            return id;
        }

        constexpr explicit operator bool() const noexcept {
            return id != 0;
        }

        auto operator<=>(const SessionId&) const = default;

    private:
        std::uint64_t id = 0;
    };

    class PendingRequestId {
    public:
        constexpr PendingRequestId() noexcept = default;
        explicit constexpr PendingRequestId(std::uint64_t value) noexcept
            : id(value) {
        }

        constexpr std::uint64_t value() const noexcept {
            return id;
        }

        constexpr explicit operator bool() const noexcept {
            return id != 0;
        }

        auto operator<=>(const PendingRequestId&) const = default;

    private:
        std::uint64_t id = 0;
    };

    class SequenceNumber {
    public:
        constexpr SequenceNumber() noexcept = default;
        explicit constexpr SequenceNumber(std::uint64_t value) noexcept
            : sequence(value) {
        }

        constexpr std::uint64_t value() const noexcept {
            return sequence;
        }

        auto operator<=>(const SequenceNumber&) const = default;

    private:
        std::uint64_t sequence = 0;
    };

    enum class ProviderLifecycle { Stopped, Starting, Initializing, Ready, Stopping, Failed, Recovering };
    enum class RecoveryStatus { Idle, Waiting, Exhausted };
    enum class Freshness { Unknown, Current, Stale };
    enum class SessionRole { Observer, Controller };
    enum class ItemLifecycle { Unknown, Started, Completed, Failed };

    struct RecoveryState {
        RecoveryStatus status = RecoveryStatus::Idle;
        std::uint32_t attempts = 0;
        std::optional<std::uint64_t> delayMs;

        bool operator==(const RecoveryState&) const = default;
    };

    struct ProviderState {
        ProviderLifecycle lifecycle = ProviderLifecycle::Stopped;
        std::uint64_t generation = 0;
        bool desiredRunning = false;
        std::optional<Error> lastError;
        RecoveryState recovery;
        std::optional<typed::InitializeResponse> initialization;
    };

    struct SourceStamp {
        std::uint64_t generation = 0;
        Freshness freshness = Freshness::Unknown;

        bool operator==(const SourceStamp&) const = default;
    };

    using ProviderOperationValue = std::variant<typed::Unit,
                                                typed::CancelLoginAccountResponse,
                                                typed::LoginAccountResponse,
                                                typed::ConsumeAccountRateLimitResetCreditResponse,
                                                typed::ConfigReadResponse,
                                                typed::ConfigRequirementsReadResponse,
                                                typed::ConfigWriteResponse,
                                                typed::ExperimentalFeatureEnablementSetResponse,
                                                typed::ExperimentalFeatureListResponse,
                                                typed::GetAccountRateLimitsResponse,
                                                typed::GetAccountResponse,
                                                typed::SendAddCreditsNudgeEmailResponse,
                                                typed::GetAccountTokenUsageResponse,
                                                typed::GetWorkspaceMessagesResponse,
                                                typed::ModelListResponse,
                                                typed::ModelProviderCapabilitiesReadResponse,
                                                typed::ThreadForkResponse,
                                                typed::ThreadGoalClearResponse,
                                                typed::ThreadGoalGetResponse,
                                                typed::ThreadGoalSetResponse,
                                                typed::ThreadListResponse,
                                                typed::ThreadLoadedListResponse,
                                                typed::ThreadMetadataUpdateResponse,
                                                typed::ThreadReadResponse,
                                                typed::ThreadResumeResponse,
                                                typed::ThreadRollbackResponse,
                                                typed::ThreadStartResponse,
                                                typed::ThreadUnarchiveResponse,
                                                typed::ThreadUnsubscribeResponse,
                                                typed::TurnStartResponse,
                                                typed::TurnSteerResponse,
                                                typed::CommandExecResponse,
                                                typed::FsGetMetadataResponse,
                                                typed::FsReadDirectoryResponse,
                                                typed::FsReadFileResponse,
                                                typed::FsWatchResponse,
                                                typed::FuzzyFileSearchResponse,
                                                typed::PermissionProfileListResponse,
                                                typed::ReviewStartResponse,
                                                typed::AppsListResponse,
                                                typed::ExternalAgentConfigDetectResponse,
                                                typed::ExternalAgentConfigImportResponse,
                                                typed::ExternalAgentConfigImportHistoriesReadResponse,
                                                typed::FeedbackUploadResponse,
                                                typed::HooksListResponse,
                                                typed::MarketplaceAddResponse,
                                                typed::MarketplaceRemoveResponse,
                                                typed::MarketplaceUpgradeResponse,
                                                typed::PluginInstallResponse,
                                                typed::PluginShareCheckoutResponse,
                                                typed::PluginShareSaveResponse,
                                                typed::PluginShareUpdateTargetsResponse,
                                                typed::PluginSkillReadResponse,
                                                typed::SkillsConfigWriteResponse,
                                                typed::SkillsListResponse,
                                                typed::PluginInstalledResponse,
                                                typed::PluginListResponse,
                                                typed::PluginReadResponse,
                                                typed::PluginShareListResponse,
                                                typed::McpServerOauthLoginResponse,
                                                typed::McpResourceReadResponse,
                                                typed::McpServerToolCallResponse,
                                                typed::ListMcpServerStatusResponse,
                                                typed::WindowsSandboxReadinessResponse,
                                                typed::WindowsSandboxSetupStartResponse>;

    static_assert(std::variant_size_v<ProviderOperationValue> == 65);

    struct BackendCapacityOptions {
        std::size_t maxSessions = 128;
        std::size_t maxObservers = 16;
        std::size_t maxActiveOperations = 4096;
        std::size_t maxPendingRequests = 1024;
        std::size_t maxRetainedThreads = 2048;
        std::size_t maxRetainedTurns = 16384;
        std::size_t maxRetainedItems = 65536;
        std::size_t maxAccumulatedContentBytes = 64U * 1024U * 1024U;
        std::size_t maxSnapshotBytes = 8U * 1024U * 1024U;
        std::size_t maxRetainedNotices = 256;
        std::size_t maxRetainedProcesses = 256;
        std::size_t maxProcessOutputBytesPerProcess = 4U * 1024U * 1024U;
        std::size_t maxAccumulatedProcessOutputBytes = 16U * 1024U * 1024U;
        std::size_t maxRetainedFilesystemWatches = 1024;
        std::size_t maxRetainedFuzzySearchSessions = 256;
        std::size_t maxRetainedActivityRecords = 512;

        bool operator==(const BackendCapacityOptions&) const = default;
    };

    struct CapacityState {
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
        std::uint64_t rejectedSessions = 0;
        std::uint64_t rejectedObservers = 0;
        std::uint64_t rejectedOperations = 0;
        std::uint64_t providerRequestOverflows = 0;
        std::uint64_t evictedThreads = 0;
        std::uint64_t evictedTurns = 0;
        std::uint64_t evictedItems = 0;
        std::uint64_t droppedContentBytes = 0;
        std::uint64_t snapshotOmissions = 0;
        std::uint64_t evictedNotices = 0;
        std::uint64_t evictedProcesses = 0;
        std::uint64_t droppedProcessOutputBytes = 0;
        std::uint64_t evictedFilesystemWatches = 0;
        std::uint64_t evictedFuzzySearchSessions = 0;
        std::uint64_t evictedActivityRecords = 0;
        BackendCapacityOptions limits;

        bool operator==(const CapacityState&) const = default;
    };

    struct DiagnosticSummary {
        std::uint64_t received = 0;
        std::vector<std::string> recent;
    };

    struct ModelRerouteRecord {
        typed::ModelId from;
        typed::ModelId to;
        std::string reason;
    };

    struct RealtimeThreadState {
        std::string lifecycle = "stopped";
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
    };

    struct ItemState {
        typed::ThreadItem item;
        ItemLifecycle lifecycle = ItemLifecycle::Unknown;
        std::string agentText;
        std::string reasoningText;
        std::string reasoningSummary;
        std::string commandOutput;
        std::uint64_t droppedContentBytes = 0;
        std::optional<std::int64_t> startedAtMs;
        std::optional<std::int64_t> completedAtMs;
        Json extensions = Json::object();
        SourceStamp stamp;
        bool connectionInvalidated = false;
    };

    struct TurnState {
        typed::Turn turn;
        std::map<std::string, ItemState> items;
        std::vector<typed::ItemId> itemOrder;
        bool active = false;
        bool terminal = false;
        std::optional<Json> failure;
        std::optional<Json> tokenUsage;
        std::optional<typed::ThreadSettings> effectiveExecutionConfiguration;
        std::optional<std::string> effectiveExecutionConfigurationProvenance;
        std::vector<ModelRerouteRecord> modelReroutes;
        Json extensions = Json::object();
        SourceStamp stamp;
        bool connectionInvalidated = false;
    };

    struct ThreadState {
        typed::Thread thread;
        std::optional<typed::ThreadSettings> executionConfiguration;
        std::optional<bool> archived;
        std::map<std::string, TurnState> turns;
        std::vector<typed::TurnId> turnOrder;
        bool fullyLoaded = false;
        Json extensions = Json::object();
        RealtimeThreadState realtime;
        SourceStamp stamp;
    };

    struct PendingRequestState {
        PendingRequestId id;
        typed::TypedServerRequest request;
        std::uint64_t connectionGeneration = 0;
    };

    struct ConnectedSessionState {
        SessionId id;
        SessionRole role = SessionRole::Observer;
    };

    struct ThreadListState {
        bool hasLoadedPage = false;
        bool complete = false;
        std::optional<std::string> nextCursor;
        std::optional<std::string> backwardsCursor;
        std::size_t pagesLoaded = 0;
        SourceStamp stamp;
    };

    struct ProviderOperationState {
        std::string method;
        std::size_t resultAlternative = 0;
        SourceStamp stamp;
    };

    struct ProviderResultSummaryState {
        std::string method;
        std::size_t resultAlternative = 0;
        std::string status = "completed";
        std::optional<std::string> subjectId;
        std::optional<std::string> nextCursor;
        std::size_t itemCount = 0;
        bool complete = true;
        SourceStamp stamp;
    };

    struct ProviderNotificationState {
        std::string method;
        std::size_t eventAlternative = 0;
        SourceStamp stamp;
    };

    struct ProviderDomainState {
        std::map<std::string, ProviderNotificationState> latestNotifications;
        std::map<std::string, ProviderResultSummaryState> latestResults;
    };

    template <typename T>
    struct ReplacementCache {
        T value;
        std::optional<std::string> requestedCursor;
        std::optional<std::string> nextCursor;
        std::size_t originalEntries = 0;
        bool truncated = false;
        SourceStamp stamp;
    };

    struct AccountLoginFlowState {
        std::string lifecycle = "idle";
        std::string method;
        std::optional<typed::LoginId> loginId;
        std::optional<std::string> cancellationStatus;
        std::optional<bool> success;
        std::optional<std::string> error;
        SourceStamp stamp;

        bool operator==(const AccountLoginFlowState&) const = default;
    };

    struct AccountAuthenticationState {
        bool authenticated = false;
        std::optional<std::string> accountType;
        std::optional<std::string> authMode;
        std::optional<std::string> planType;
        SourceStamp stamp;

        bool operator==(const AccountAuthenticationState&) const = default;
    };

    struct AccountRateLimitState {
        std::optional<std::string> planType;
        std::optional<std::string> reachedType;
        std::optional<std::int32_t> primaryUsedPercent;
        std::optional<std::int64_t> primaryResetsAt;
        std::optional<std::int32_t> secondaryUsedPercent;
        std::optional<std::int64_t> secondaryResetsAt;
        std::optional<bool> hasCredits;
        std::optional<bool> unlimitedCredits;
        std::optional<std::string> creditBalance;
        SourceStamp stamp;

        bool operator==(const AccountRateLimitState&) const = default;
    };

    struct AccountDomainState : ProviderDomainState {
        std::optional<ReplacementCache<typed::CancelLoginAccountResponse>> loginCancellation;
        std::optional<ReplacementCache<typed::LoginAccountResponse>> loginStart;
        std::optional<ReplacementCache<typed::GetAccountRateLimitsResponse>> rateLimitRead;
        std::optional<ReplacementCache<typed::GetAccountResponse>> accountRead;
        std::optional<ReplacementCache<typed::GetAccountTokenUsageResponse>> usage;
        std::optional<ReplacementCache<typed::GetWorkspaceMessagesResponse>> workspaceMessages;
        std::optional<AccountLoginFlowState> login;
        std::optional<AccountAuthenticationState> authentication;
        std::optional<AccountRateLimitState> rateLimits;
        std::optional<std::string> resetCreditOutcome;
        SourceStamp resetCreditStamp;
        bool loggedOut = false;
        SourceStamp logoutStamp;
    };

    struct AppCatalogEntryState {
        std::string id;
        std::string name;
        std::optional<bool> accessible;
        std::optional<bool> enabled;

        bool operator==(const AppCatalogEntryState&) const = default;
    };

    struct AppCatalogState {
        std::vector<AppCatalogEntryState> entries;
        std::size_t totalEntries = 0;
        bool truncated = false;
        SourceStamp stamp;

        bool operator==(const AppCatalogState&) const = default;
    };

    struct IntegrationsDomainState : ProviderDomainState {
        std::optional<ReplacementCache<typed::AppsListResponse>> appList;
        std::optional<ReplacementCache<typed::ExternalAgentConfigDetectResponse>> externalAgentDetection;
        std::optional<ReplacementCache<typed::ExternalAgentConfigImportResponse>> externalAgentImport;
        std::optional<ReplacementCache<typed::ExternalAgentConfigImportHistoriesReadResponse>> externalAgentImportHistories;
        std::optional<ReplacementCache<typed::HooksListResponse>> hooks;
        std::optional<ReplacementCache<typed::MarketplaceAddResponse>> marketplaceAdd;
        std::optional<ReplacementCache<typed::MarketplaceRemoveResponse>> marketplaceRemove;
        std::optional<ReplacementCache<typed::MarketplaceUpgradeResponse>> marketplaceUpgrade;
        std::optional<AppCatalogState> apps;
    };

    struct ModelsDomainState : ProviderDomainState {
        std::optional<ReplacementCache<typed::ModelListResponse>> list;
        std::optional<ReplacementCache<typed::ModelProviderCapabilitiesReadResponse>> providerCapabilities;
    };

    struct ConfigurationDomainState : ProviderDomainState {
        std::optional<ReplacementCache<typed::ConfigReadResponse>> configuration;
        std::optional<ReplacementCache<typed::ConfigRequirementsReadResponse>> requirements;
        std::optional<ReplacementCache<typed::ExperimentalFeatureListResponse>> experimentalFeatures;
        std::optional<ReplacementCache<typed::ConfigWriteResponse>> lastWrite;
        std::optional<ReplacementCache<typed::ExperimentalFeatureEnablementSetResponse>> experimentalFeatureEnablement;
    };

    struct ConversationDomainState : ProviderDomainState {
        std::optional<typed::ThreadId> latestGoalThreadId;
        std::optional<ReplacementCache<typed::ThreadGoalGetResponse>> latestGoal;
        std::optional<typed::ThreadId> latestGoalClearThreadId;
        std::optional<ReplacementCache<typed::ThreadGoalClearResponse>> latestGoalClear;
        std::optional<typed::ThreadId> latestGoalSetThreadId;
        std::optional<ReplacementCache<typed::ThreadGoalSetResponse>> latestGoalSet;
        std::optional<typed::ThreadId> latestUnsubscribeThreadId;
        std::optional<ReplacementCache<typed::ThreadUnsubscribeResponse>> latestUnsubscribe;
        std::optional<ReplacementCache<typed::ThreadLoadedListResponse>> loadedThreads;
    };

    struct ReviewsDomainState : ProviderDomainState {
        std::optional<ReplacementCache<typed::PermissionProfileListResponse>> permissionProfiles;
        std::optional<ReplacementCache<typed::ReviewStartResponse>> latestReview;
    };

    struct SkillsExtraRootsState {
        std::vector<typed::AbsolutePath> roots;
        std::size_t totalRoots = 0;
        bool truncated = false;
        SourceStamp stamp;

        bool operator==(const SkillsExtraRootsState&) const = default;
    };

    struct PluginsAndSkillsDomainState : ProviderDomainState {
        std::optional<ReplacementCache<typed::PluginInstallResponse>> pluginInstall;
        std::optional<ReplacementCache<typed::PluginInstalledResponse>> installedPlugins;
        std::optional<ReplacementCache<typed::PluginListResponse>> plugins;
        std::optional<ReplacementCache<typed::PluginReadResponse>> pluginDetail;
        std::optional<ReplacementCache<typed::PluginShareListResponse>> pluginShares;
        std::optional<ReplacementCache<typed::PluginShareCheckoutResponse>> pluginShareCheckout;
        std::optional<ReplacementCache<typed::PluginShareSaveResponse>> pluginShareSave;
        std::optional<ReplacementCache<typed::PluginShareUpdateTargetsResponse>> pluginShareUpdateTargets;
        std::optional<ReplacementCache<typed::PluginSkillReadResponse>> pluginSkill;
        std::optional<ReplacementCache<typed::SkillsConfigWriteResponse>> skillsConfigWrite;
        std::optional<ReplacementCache<typed::SkillsListResponse>> skills;
        std::optional<SkillsExtraRootsState> extraRoots;
    };

    struct McpOauthState {
        std::string serverName;
        std::string lifecycle = "idle";
        std::optional<bool> success;
        std::optional<std::string> error;
        std::optional<typed::ThreadId> threadId;
        SourceStamp stamp;

        bool operator==(const McpOauthState&) const = default;
    };

    struct McpStartupState {
        std::string serverName;
        std::string status;
        std::optional<std::string> error;
        std::optional<std::string> failureReason;
        std::optional<typed::ThreadId> threadId;
        SourceStamp stamp;

        bool operator==(const McpStartupState&) const = default;
    };

    struct McpStatusListState {
        std::size_t serverCount = 0;
        std::optional<std::string> nextCursor;
        bool complete = false;
        SourceStamp stamp;

        bool operator==(const McpStatusListState&) const = default;
    };

    struct McpDomainState : ProviderDomainState {
        std::optional<ReplacementCache<typed::McpServerOauthLoginResponse>> oauthStart;
        std::optional<ReplacementCache<typed::ListMcpServerStatusResponse>> statusListResponse;
        std::optional<McpOauthState> oauth;
        std::optional<McpStartupState> startup;
        std::optional<McpStatusListState> statusList;
    };

    struct RemoteControlState {
        std::string status;
        std::optional<std::string> environmentId;
        std::string installationId;
        std::string serverName;
        SourceStamp stamp;

        bool operator==(const RemoteControlState&) const = default;
    };

    struct WindowsSandboxState {
        std::string lifecycle = "idle";
        std::optional<std::string> readiness;
        std::optional<std::string> mode;
        std::optional<bool> success;
        std::optional<std::string> error;
        SourceStamp stamp;

        bool operator==(const WindowsSandboxState&) const = default;
    };

    struct PlatformDomainState : ProviderDomainState {
        std::optional<ReplacementCache<typed::WindowsSandboxReadinessResponse>> windowsReadiness;
        std::optional<RemoteControlState> remoteControl;
        std::optional<WindowsSandboxState> windowsSandbox;
    };

    enum class NoticeCategory { Warning, Deprecation, Configuration, Security, WindowsWorldWritable };

    struct NoticeState {
        std::uint64_t occurrence = 0;
        NoticeCategory category = NoticeCategory::Warning;
        std::string summary;
        std::optional<std::string> details;
        std::optional<typed::ThreadId> threadId;
        SourceStamp stamp;
    };

    struct ProcessState {
        std::string processHandle;
        std::string lifecycle = "running";
        std::string stdoutData;
        std::string stderrData;
        bool stdoutTruncated = false;
        bool stderrTruncated = false;
        std::uint64_t droppedOutputBytes = 0;
        std::optional<std::int32_t> exitCode;
        SourceStamp stamp;
        bool connectionInvalidated = false;
    };

    struct FilesystemWatchState {
        typed::FsWatchId watchId;
        std::optional<typed::AbsolutePath> root;
        std::vector<typed::AbsolutePath> changedPaths;
        SourceStamp stamp;
        bool connectionInvalidated = false;
    };

    struct FuzzySearchState {
        std::string sessionId;
        std::string query;
        std::vector<typed::FuzzyFileSearchResult> files;
        bool complete = false;
        SourceStamp stamp;
        bool connectionInvalidated = false;
    };

    struct ActivityRecordState {
        std::string key;
        std::string subjectId;
        std::string kind;
        std::string lifecycle;
        std::optional<std::string> summary;
        std::optional<std::string> details;
        std::optional<typed::ThreadId> threadId;
        std::optional<typed::TurnId> turnId;
        ProviderNotificationState notification;
        bool active = false;
    };

    struct ExtensionRecord {
        std::string method;
        Json payload = nullptr;
        std::optional<std::string> decodingError;
        std::optional<std::uint64_t> originalMethodBytes;
        std::optional<std::uint64_t> originalPayloadBytes;
        std::optional<std::uint64_t> originalDecodingErrorBytes;
        std::optional<typed::DecodeDiagnostic> diagnostic = std::nullopt;
        std::optional<std::uint64_t> originalDiagnosticBytes = std::nullopt;
    };

    struct BackendState {
        ProviderState provider;
        CapacityState capacity;
        DiagnosticSummary diagnostics;
        std::map<std::string, ThreadState> threads;
        std::vector<typed::ThreadId> threadOrder;
        std::map<PendingRequestId, PendingRequestState> pendingRequests;
        std::map<SessionId, ConnectedSessionState> sessions;
        std::optional<SessionId> controller;
        SequenceNumber sequence;
        bool sequenceExhausted = false;
        ThreadListState threadList;
        std::map<std::string, ProviderOperationState> providerOperations;
        AccountDomainState accounts;
        ModelsDomainState models;
        ConfigurationDomainState configuration;
        ConversationDomainState conversations;
        ProviderDomainState filesystem;
        ReviewsDomainState reviews;
        IntegrationsDomainState integrations;
        PluginsAndSkillsDomainState pluginsAndSkills;
        McpDomainState mcp;
        PlatformDomainState platform;
        std::map<std::string, ProcessState> processes;
        std::vector<std::string> processOrder;
        std::set<std::string> processReservations;
        std::map<std::string, std::string> processReservationTargets;
        std::map<std::string, std::string> processReservationClaims;
        std::map<std::string, FilesystemWatchState> filesystemWatches;
        std::vector<std::string> filesystemWatchOrder;
        std::set<std::string> filesystemWatchReservations;
        std::map<std::string, std::string> filesystemWatchReservationTargets;
        std::map<std::string, std::string> filesystemWatchReservationClaims;
        std::map<std::string, FuzzySearchState> fuzzySearchSessions;
        std::vector<std::string> fuzzySearchOrder;
        std::set<std::string> fuzzySearchReservations;
        std::map<std::string, std::string> fuzzySearchReservationTargets;
        std::map<std::string, std::string> fuzzySearchReservationClaims;
        std::map<std::string, ActivityRecordState> activities;
        std::vector<std::string> activityOrder;
        std::vector<NoticeState> notices;
        std::uint64_t nextNoticeOccurrence = 1;
        std::vector<ExtensionRecord> recentExtensions;
    };

    ProviderLifecycle toProviderLifecycle(State state) noexcept;
    bool isTerminal(const typed::TurnStatus& status) noexcept;
    std::optional<typed::ItemId> itemId(const typed::ThreadItem& item);
    std::string itemType(const typed::ThreadItem& item);

    ThreadState* findThread(BackendState& state, const typed::ThreadId& threadId) noexcept;
    const ThreadState* findThread(const BackendState& state, const typed::ThreadId& threadId) noexcept;
    TurnState* findTurn(BackendState& state, const typed::ThreadId& threadId, const typed::TurnId& turnId) noexcept;
    const TurnState* findTurn(const BackendState& state, const typed::ThreadId& threadId, const typed::TurnId& turnId) noexcept;
    ItemState*
    findItem(BackendState& state, const typed::ThreadId& threadId, const typed::TurnId& turnId, const typed::ItemId& itemId) noexcept;
    const ItemState*
    findItem(const BackendState& state, const typed::ThreadId& threadId, const typed::TurnId& turnId, const typed::ItemId& itemId) noexcept;

} // namespace ai::openai::codex::backend

#endif // AI_OPENAI_CODEX_BACKEND_BACKENDSTATE_H
