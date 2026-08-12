/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_MODEL_H
#define AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_MODEL_H

#include "ai/openai/codex/frontend/Messages.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ai::openai::codex::frontend::internal::model {

    template <typename Tag>
    class StrongIdentifier {
    public:
        static constexpr std::size_t MaximumBytes = 1024;

        explicit StrongIdentifier(std::string value)
            : identifier(std::move(value)) {
            if (!isValid(identifier)) {
                throw std::invalid_argument("frontend identifier is empty, oversized, or contains NUL");
            }
        }

        [[nodiscard]] static std::optional<StrongIdentifier> parse(std::string value) noexcept {
            try {
                if (!isValid(value)) {
                    return std::nullopt;
                }
                return StrongIdentifier(std::move(value));
            } catch (...) {
                return std::nullopt;
            }
        }

        [[nodiscard]] const std::string& value() const noexcept {
            return identifier;
        }

        [[nodiscard]] static bool isValid(std::string_view value) noexcept {
            return !value.empty() && value.size() <= MaximumBytes && value.find('\0') == std::string_view::npos;
        }

        auto operator<=>(const StrongIdentifier&) const = default;

    private:
        std::string identifier;
    };

    struct SessionIdentityTag;
    struct ControllerIdentityTag;
    struct ThreadIdentityTag;
    struct TurnIdentityTag;
    struct ItemIdentityTag;
    struct PendingRequestIdentityTag;
    struct ProcessHandleTag;
    struct ProjectionStampTag;
    struct SourceStampTag;
    struct OccurrenceGroupIdentityTag;

    using SessionIdentity = StrongIdentifier<SessionIdentityTag>;
    using ControllerIdentity = StrongIdentifier<ControllerIdentityTag>;
    using ThreadIdentity = StrongIdentifier<ThreadIdentityTag>;
    using TurnIdentity = StrongIdentifier<TurnIdentityTag>;
    using ItemIdentity = StrongIdentifier<ItemIdentityTag>;
    using PendingRequestIdentity = StrongIdentifier<PendingRequestIdentityTag>;
    using ProcessHandle = StrongIdentifier<ProcessHandleTag>;
    using ProjectionStamp = StrongIdentifier<ProjectionStampTag>;
    using SourceStamp = StrongIdentifier<SourceStampTag>;
    using OccurrenceGroupIdentity = StrongIdentifier<OccurrenceGroupIdentityTag>;

    class FrontendSequence {
    public:
        using Value = SequenceNumber::Value;

        constexpr FrontendSequence() noexcept = default;

        explicit constexpr FrontendSequence(Value value) noexcept
            : sequence(value) {
        }

        explicit constexpr FrontendSequence(SequenceNumber value) noexcept
            : sequence(value.value()) {
        }

        [[nodiscard]] constexpr Value value() const noexcept {
            return sequence;
        }

        [[nodiscard]] constexpr SequenceNumber protocolValue() const noexcept {
            return SequenceNumber(sequence);
        }

        [[nodiscard]] static constexpr FrontendSequence maximum() noexcept {
            return FrontendSequence(SequenceNumber::maximum());
        }

        auto operator<=>(const FrontendSequence&) const = default;

    private:
        Value sequence = 0;
    };

    struct SafeDetailLimits {
        std::size_t maximumBytes = 64U * 1024U;
        std::size_t maximumDepth = 16;
        std::size_t maximumMembers = 512;

        bool operator==(const SafeDetailLimits&) const = default;
    };

    enum class SafeDetailError { None, ByteLimit, DepthLimit, MemberLimit, SecretKey, UnsupportedValue, InvalidLimits };

    class SafeDetail {
    public:
        static constexpr std::size_t HardMaximumBytes = 64U * 1024U;
        static constexpr std::size_t HardMaximumDepth = 16;
        static constexpr std::size_t HardMaximumMembers = 512;

        SafeDetail();

        [[nodiscard]] static std::optional<SafeDetail>
        fromJson(Json value, SafeDetailError* error = nullptr, SafeDetailLimits limits = {}) noexcept;

        [[nodiscard]] const Json& json() const noexcept;
        [[nodiscard]] std::size_t serializedBytes() const noexcept;
        [[nodiscard]] bool empty() const noexcept;
        [[nodiscard]] static bool isSecretKey(std::string_view key) noexcept;

        bool operator==(const SafeDetail&) const = default;

    private:
        SafeDetail(Json value, std::size_t serializedBytes);

        Json detail;
        std::size_t bytes = 0;
    };

    enum class InformationState { Present, Omitted, Redacted, Truncated, Unavailable, Stale, Unknown, Absent, NullValue };
    enum class Freshness { Unknown, Current, Stale };

    struct SourceMetadata {
        std::uint64_t generation = 0;
        Freshness freshness = Freshness::Unknown;
        SafeDetail extensions;
        bool operator==(const SourceMetadata&) const = default;
    };

    struct TruncationMetadata {
        bool truncated = false;
        std::optional<std::size_t> omittedEntries;
        std::uint64_t droppedBytes = 0;
        bool droppedBytesPresent = false;
        std::vector<std::string> omittedPaths;
        SafeDetail extensions;

        bool operator==(const TruncationMetadata&) const = default;
    };

    struct ProjectionMetadata {
        std::optional<ProjectionStamp> projectionStamp;
        std::optional<SourceStamp> sourceStamp;
        std::vector<std::string> omittedPaths;
        std::vector<std::string> redactedPaths;
        std::vector<std::string> truncatedPaths;
        std::vector<std::string> unavailablePaths;
        std::vector<std::string> stalePaths;
        std::vector<std::string> unknownPaths;
        std::vector<std::string> absentPaths;
        std::vector<std::string> nullPaths;

        bool operator==(const ProjectionMetadata&) const = default;
    };

    struct DomainResultSummary {
        std::string method;
        std::string status;
        std::optional<std::string> subjectId;
        std::optional<std::string> nextCursor;
        std::optional<std::uint64_t> itemCount;
        bool complete = false;
        bool completeKnown = false;
        SourceMetadata stamp;
        SafeDetail extensions;

        bool operator==(const DomainResultSummary&) const = default;
    };

    struct FilesystemWatchRecord {
        std::string watchId;
        std::optional<std::string> root;
        std::optional<std::uint64_t> changedPathCount;
        SourceMetadata stamp;
        bool connectionInvalidated = false;
        TruncationMetadata truncation;
        SafeDetail safeDetails;
        SafeDetail extensions;
        SafeDetail publicExtensions;
        bool publicExtensionsKnown = false;
        bool operator==(const FilesystemWatchRecord&) const = default;
    };

    struct FuzzySearchRecord {
        std::string sessionId;
        std::optional<std::uint64_t> resultCount;
        bool complete = false;
        SourceMetadata stamp;
        bool connectionInvalidated = false;
        TruncationMetadata truncation;
        SafeDetail safeDetails;
        SafeDetail extensions;
        SafeDetail publicExtensions;
        bool publicExtensionsKnown = false;
        bool operator==(const FuzzySearchRecord&) const = default;
    };

    struct NoticeRecord {
        std::uint64_t occurrence = 0;
        std::string category;
        std::string summary;
        std::optional<std::string> details;
        std::optional<ThreadIdentity> threadId;
        SourceMetadata stamp;
        SafeDetail safeDetails;
        SafeDetail extensions;
        bool operator==(const NoticeRecord&) const = default;
    };

    struct ActivityRecord {
        std::string key;
        std::string subjectId;
        std::string kind;
        std::string lifecycle;
        std::optional<std::string> summary;
        std::optional<std::string> details;
        bool active = false;
        std::optional<ThreadIdentity> threadId;
        std::optional<TurnIdentity> turnId;
        SourceMetadata stamp;
        SafeDetail safeDetails;
        SafeDetail extensions;
        bool operator==(const ActivityRecord&) const = default;
    };

    struct DiagnosticRecord {
        std::optional<std::uint64_t> received;
        bool detailsOmitted = false;
        std::optional<std::string> message;
        SafeDetail safeDetails;
        SafeDetail extensions;
        bool operator==(const DiagnosticRecord&) const = default;
    };

    struct DomainState {
        InformationState information = InformationState::Absent;
        std::optional<std::string> status;
        std::optional<std::string> summary;
        std::optional<std::string> nextCursor;
        std::optional<std::uint64_t> itemCount;
        bool complete = false;
        bool completeKnown = false;
        SourceMetadata stamp;
        bool stampKnown = true;
        std::vector<DomainResultSummary> latestResults;
        bool latestResultsKnown = true;
        TruncationMetadata truncation;
        bool truncationKnown = true;
        SafeDetail safeDetails;
        bool safeDetailsKnown = true;
        SafeDetail extensions;

        [[nodiscard]] static DomainState present(Freshness freshness = Freshness::Current);
        [[nodiscard]] bool valid() const noexcept;

        bool operator==(const DomainState&) const = default;
    };

#define AISUITE_CODEX_FRONTEND_DOMAIN_STATE(name)                                                                                          \
    struct name {                                                                                                                          \
        DomainState state;                                                                                                                 \
        bool operator==(const name&) const = default;                                                                                      \
    }

    AISUITE_CODEX_FRONTEND_DOMAIN_STATE(AccountsState);
    AISUITE_CODEX_FRONTEND_DOMAIN_STATE(ModelsState);
    AISUITE_CODEX_FRONTEND_DOMAIN_STATE(ConfigurationState);
    AISUITE_CODEX_FRONTEND_DOMAIN_STATE(PermissionProfilesState);
    AISUITE_CODEX_FRONTEND_DOMAIN_STATE(ReviewsState);
    AISUITE_CODEX_FRONTEND_DOMAIN_STATE(AppsState);
    AISUITE_CODEX_FRONTEND_DOMAIN_STATE(ExternalAgentsState);
    AISUITE_CODEX_FRONTEND_DOMAIN_STATE(HooksState);
    AISUITE_CODEX_FRONTEND_DOMAIN_STATE(MarketplaceState);
    AISUITE_CODEX_FRONTEND_DOMAIN_STATE(PluginsState);
    AISUITE_CODEX_FRONTEND_DOMAIN_STATE(SkillsState);
    AISUITE_CODEX_FRONTEND_DOMAIN_STATE(McpState);
    AISUITE_CODEX_FRONTEND_DOMAIN_STATE(WindowsSandboxState);
    AISUITE_CODEX_FRONTEND_DOMAIN_STATE(PlatformState);
    AISUITE_CODEX_FRONTEND_DOMAIN_STATE(RemoteControlState);
    AISUITE_CODEX_FRONTEND_DOMAIN_STATE(IntegrationsState);

#undef AISUITE_CODEX_FRONTEND_DOMAIN_STATE

    struct FilesystemWatchesState {
        DomainState state;
        std::vector<FilesystemWatchRecord> entries;
        bool operator==(const FilesystemWatchesState&) const = default;
    };

    struct FuzzySearchesState {
        DomainState state;
        std::vector<FuzzySearchRecord> entries;
        bool operator==(const FuzzySearchesState&) const = default;
    };

    struct NoticesState {
        DomainState state;
        std::vector<NoticeRecord> entries;
        bool operator==(const NoticesState&) const = default;
    };

    struct ActivitiesState {
        DomainState state;
        std::vector<ActivityRecord> entries;
        bool operator==(const ActivitiesState&) const = default;
    };

    struct DiagnosticsState {
        DomainState state;
        std::optional<std::uint64_t> received;
        std::vector<DiagnosticRecord> entries;
        bool operator==(const DiagnosticsState&) const = default;
    };

    enum class ProviderLifecycle { Stopped, Starting, Initializing, Ready, Stopping, Failed, Recovering };
    enum class ProviderRecoveryStatus { Idle, Waiting, Exhausted };

    [[nodiscard]] std::string_view toString(ProviderLifecycle lifecycle) noexcept;
    [[nodiscard]] std::optional<ProviderLifecycle> providerLifecycleFromString(std::string_view value) noexcept;
    [[nodiscard]] std::string_view toString(ProviderRecoveryStatus status) noexcept;
    [[nodiscard]] std::optional<ProviderRecoveryStatus> providerRecoveryStatusFromString(std::string_view value) noexcept;

    struct ProviderRecoveryState {
        ProviderRecoveryStatus status = ProviderRecoveryStatus::Idle;
        std::uint64_t attempts = 0;
        std::optional<std::uint64_t> delayMs;
        SafeDetail extensions;
        bool operator==(const ProviderRecoveryState&) const = default;
    };

    struct ProviderState {
        InformationState information = InformationState::Present;
        Freshness freshness = Freshness::Unknown;
        std::optional<std::string> provider;
        ProviderLifecycle lifecycle = ProviderLifecycle::Stopped;
        std::uint64_t generation = 0;
        bool desiredRunning = false;
        ProviderRecoveryState recovery;
        std::optional<SafeDetail> lastError;
        std::optional<SafeDetail> initialization;
        SafeDetail extensions;

        [[nodiscard]] bool ready() const noexcept {
            return lifecycle == ProviderLifecycle::Ready;
        }

        bool operator==(const ProviderState&) const = default;
    };

    struct ControllerState {
        InformationState information = InformationState::Present;
        std::optional<ControllerIdentity> controller;
        std::optional<SessionIdentity> session;
        SafeDetail safeDetails;

        bool operator==(const ControllerState&) const = default;
    };

    struct SessionState {
        SessionIdentity id;
        SessionRole role = SessionRole::Observer;
        std::optional<std::string> principalId;
        Freshness freshness = Freshness::Current;
        SafeDetail safeDetails;

        explicit SessionState(SessionIdentity sessionId)
            : id(std::move(sessionId)) {
        }

        bool operator==(const SessionState&) const = default;
    };

    struct ThreadListState {
        bool hasLoadedPage = false;
        bool complete = false;
        std::uint64_t pagesLoaded = 0;
        std::optional<std::string> nextCursor;
        std::optional<std::string> backwardsCursor;
        SourceMetadata stamp;
        bool stampKnown = true;
        SafeDetail safeDetails;

        bool operator==(const ThreadListState&) const = default;
    };

    struct ThreadState {
        ThreadIdentity id;
        std::optional<std::string> title;
        std::optional<std::int64_t> createdAtMs;
        std::optional<std::int64_t> updatedAtMs;
        bool fullyLoaded = false;
        Freshness freshness = Freshness::Current;
        SourceMetadata stamp;
        bool stampKnown = true;
        SafeDetail safeDetails;
        // Frontend Protocol v1 keeps the legacy nested extension object
        // distinct from the expanded record's additional safe-detail fields.
        SafeDetail legacyExtensions;

        explicit ThreadState(ThreadIdentity threadId)
            : id(std::move(threadId)) {
        }

        bool operator==(const ThreadState&) const = default;
    };

    struct TurnState {
        TurnIdentity id;
        ThreadIdentity threadId;
        std::optional<std::string> status;
        bool active = false;
        bool terminal = false;
        Freshness freshness = Freshness::Current;
        SourceMetadata stamp;
        bool stampKnown = true;
        bool connectionInvalidated = false;
        SafeDetail safeDetails;
        SafeDetail legacyExtensions;

        TurnState(TurnIdentity turnId, ThreadIdentity parentThreadId)
            : id(std::move(turnId))
            , threadId(std::move(parentThreadId)) {
        }

        bool operator==(const TurnState&) const = default;
    };

    struct ItemData {
        ItemIdentity id;
        std::optional<ThreadIdentity> threadId;
        std::optional<TurnIdentity> turnId;
        std::optional<std::string> status;
        std::optional<std::string> summary;
        std::optional<SafeDetail> location;
        std::optional<std::string> agentText;
        std::optional<std::string> reasoningText;
        std::optional<std::string> reasoningSummary;
        std::optional<std::string> commandOutput;
        std::optional<std::uint64_t> droppedContentBytes;
        bool contentTruncated = false;
        std::optional<std::int64_t> startedAtMs;
        std::optional<std::int64_t> completedAtMs;
        std::optional<SafeDetail> safeDetails;
        // Frontend Protocol v1 exposes several pre-expanded discriminators and a
        // nested extensions object.  Retain those bounded representation hints
        // without making either one a second semantic item identity.
        std::optional<std::string> legacyDiscriminator;
        SafeDetail legacyExtensions;
        TruncationMetadata truncation;
        bool connectionInvalidated = false;
        std::optional<std::uint64_t> generation;
        Freshness freshness = Freshness::Unknown;
        // Legacy snapshots may carry additive fields inside their explicit
        // stamp object.  Expanded Frontend Protocol v1 has no equivalent
        // member, so retain those fields only for the private public-State
        // compatibility adapter.
        SafeDetail stampExtensions;
        SafeDetail extensions;
        std::optional<std::size_t> sourceIndex;

        ItemData(ItemIdentity itemId,
                 std::optional<ThreadIdentity> parentThreadId = std::nullopt,
                 std::optional<TurnIdentity> parentTurnId = std::nullopt)
            : id(std::move(itemId))
            , threadId(std::move(parentThreadId))
            , turnId(std::move(parentTurnId)) {
        }

        bool operator==(const ItemData&) const = default;
    };

#define AISUITE_CODEX_FRONTEND_ITEM(name)                                                                                                  \
    struct name {                                                                                                                          \
        ItemData value;                                                                                                                    \
        bool operator==(const name&) const = default;                                                                                      \
    }

    AISUITE_CODEX_FRONTEND_ITEM(AgentMessageItem);
    AISUITE_CODEX_FRONTEND_ITEM(CollabAgentToolCallItem);
    AISUITE_CODEX_FRONTEND_ITEM(CommandExecutionItem);
    AISUITE_CODEX_FRONTEND_ITEM(ContextCompactionItem);
    AISUITE_CODEX_FRONTEND_ITEM(DynamicToolCallItem);
    AISUITE_CODEX_FRONTEND_ITEM(EnteredReviewModeItem);
    AISUITE_CODEX_FRONTEND_ITEM(ExitedReviewModeItem);
    AISUITE_CODEX_FRONTEND_ITEM(FileChangeItem);
    AISUITE_CODEX_FRONTEND_ITEM(HookPromptItem);
    AISUITE_CODEX_FRONTEND_ITEM(ImageGenerationItem);
    AISUITE_CODEX_FRONTEND_ITEM(ImageViewItem);
    AISUITE_CODEX_FRONTEND_ITEM(McpToolCallItem);
    AISUITE_CODEX_FRONTEND_ITEM(PlanItem);
    AISUITE_CODEX_FRONTEND_ITEM(ReasoningItem);
    AISUITE_CODEX_FRONTEND_ITEM(SleepItem);
    AISUITE_CODEX_FRONTEND_ITEM(SubAgentActivityItem);
    AISUITE_CODEX_FRONTEND_ITEM(UserMessageItem);
    AISUITE_CODEX_FRONTEND_ITEM(WebSearchItem);

#undef AISUITE_CODEX_FRONTEND_ITEM

    using ThreadItem = std::variant<AgentMessageItem,
                                    CollabAgentToolCallItem,
                                    CommandExecutionItem,
                                    ContextCompactionItem,
                                    DynamicToolCallItem,
                                    EnteredReviewModeItem,
                                    ExitedReviewModeItem,
                                    FileChangeItem,
                                    HookPromptItem,
                                    ImageGenerationItem,
                                    ImageViewItem,
                                    McpToolCallItem,
                                    PlanItem,
                                    ReasoningItem,
                                    SleepItem,
                                    SubAgentActivityItem,
                                    UserMessageItem,
                                    WebSearchItem>;

    [[nodiscard]] ThreadItemKind threadItemKind(const ThreadItem& item) noexcept;
    [[nodiscard]] const ItemData& itemData(const ThreadItem& item) noexcept;

    struct LegacyItemCompatibility {
        ItemData value;
        std::string discriminator;
        std::size_t sourceIndex = 0;
        std::string omissionPath;

        bool operator==(const LegacyItemCompatibility&) const = default;
    };

    struct PendingRequestOption {
        std::string label;
        std::string description;
        SafeDetail extensions;

        bool operator==(const PendingRequestOption&) const = default;
    };

    struct PendingRequestQuestion {
        std::string id;
        std::string header;
        std::string prompt;
        bool allowsFreeText = false;
        bool secretAnswer = false;
        std::vector<PendingRequestOption> options;
        SafeDetail extensions;

        bool operator==(const PendingRequestQuestion&) const = default;
    };

    struct PendingRequestData {
        PendingRequestIdentity id;
        std::optional<ThreadIdentity> threadId;
        std::optional<TurnIdentity> turnId;
        std::optional<ItemIdentity> itemId;
        std::optional<std::string> summary;
        std::optional<SafeDetail> safeDetails;
        bool questionsPresent = false;
        std::vector<PendingRequestQuestion> questions;
        std::optional<std::uint64_t> autoResolutionMs;
        // A retained request from an older physical connection is display-only:
        // reverse responses must never be submitted against its old generation.
        bool connectionInvalidated = false;
        TruncationMetadata truncation;
        SafeDetail extensions;
        std::optional<std::size_t> sourceIndex;

        PendingRequestData(PendingRequestIdentity requestId,
                           std::optional<ThreadIdentity> parentThreadId = std::nullopt,
                           std::optional<TurnIdentity> parentTurnId = std::nullopt,
                           std::optional<ItemIdentity> parentItemId = std::nullopt)
            : id(std::move(requestId))
            , threadId(std::move(parentThreadId))
            , turnId(std::move(parentTurnId))
            , itemId(std::move(parentItemId)) {
        }

        bool operator==(const PendingRequestData&) const = default;
    };

#define AISUITE_CODEX_FRONTEND_PENDING(name)                                                                                               \
    struct name {                                                                                                                          \
        PendingRequestData value;                                                                                                          \
        bool operator==(const name&) const = default;                                                                                      \
    }

    AISUITE_CODEX_FRONTEND_PENDING(CommandExecutionApprovalRequest);
    AISUITE_CODEX_FRONTEND_PENDING(FileChangeApprovalRequest);
    AISUITE_CODEX_FRONTEND_PENDING(UserInputRequest);
    AISUITE_CODEX_FRONTEND_PENDING(AuthenticationRequest);
    AISUITE_CODEX_FRONTEND_PENDING(ApplyPatchApprovalRequest);
    AISUITE_CODEX_FRONTEND_PENDING(ExecCommandApprovalRequest);
    AISUITE_CODEX_FRONTEND_PENDING(PermissionsApprovalRequest);
    AISUITE_CODEX_FRONTEND_PENDING(AttestationRequest);
    AISUITE_CODEX_FRONTEND_PENDING(DynamicToolCallRequest);
    AISUITE_CODEX_FRONTEND_PENDING(McpElicitationRequest);

#undef AISUITE_CODEX_FRONTEND_PENDING

    using PendingRequest = std::variant<CommandExecutionApprovalRequest,
                                        FileChangeApprovalRequest,
                                        UserInputRequest,
                                        AuthenticationRequest,
                                        ApplyPatchApprovalRequest,
                                        ExecCommandApprovalRequest,
                                        PermissionsApprovalRequest,
                                        AttestationRequest,
                                        DynamicToolCallRequest,
                                        McpElicitationRequest>;

    [[nodiscard]] PendingRequestKind pendingRequestKind(const PendingRequest& request) noexcept;
    [[nodiscard]] const PendingRequestData& pendingRequestData(const PendingRequest& request) noexcept;

    struct LegacyPendingRequestCompatibility {
        PendingRequestData value;
        std::size_t sourceIndex = 0;
        std::string omissionPath;
        bool operator==(const LegacyPendingRequestCompatibility&) const = default;
    };
    [[nodiscard]] const PendingRequestData& pendingRequestData(const PendingRequest& request) noexcept;

    struct ProcessState {
        ProcessHandle handle;
        std::optional<std::int64_t> processId;
        std::optional<std::string> status;
        std::optional<std::string> lifecycle;
        std::optional<std::uint64_t> stdoutBytes;
        std::optional<std::uint64_t> stderrBytes;
        bool stdoutTruncated = false;
        bool stderrTruncated = false;
        std::optional<std::uint64_t> droppedOutputBytes;
        SourceMetadata stamp;
        bool connectionInvalidated = false;
        std::optional<std::int64_t> exitCode;
        bool terminal = false;
        TruncationMetadata truncation;
        SafeDetail safeDetails;
        SafeDetail extensions;
        SafeDetail publicExtensions;
        bool publicExtensionsKnown = false;

        explicit ProcessState(ProcessHandle processHandle)
            : handle(std::move(processHandle)) {
        }

        bool operator==(const ProcessState&) const = default;
    };

    struct CapacityState {
        std::optional<std::size_t> sessions;
        std::optional<std::size_t> observers;
        std::optional<std::size_t> activeOperations;
        std::optional<std::size_t> pendingRequests;
        std::optional<std::size_t> retainedThreads;
        std::optional<std::size_t> retainedTurns;
        std::optional<std::size_t> retainedItems;
        std::optional<std::size_t> accumulatedContentBytes;
        std::optional<std::size_t> retainedNotices;
        std::optional<std::size_t> retainedProcesses;
        std::optional<std::size_t> accumulatedProcessOutputBytes;
        std::optional<std::size_t> retainedFilesystemWatches;
        std::optional<std::size_t> retainedFuzzySearchSessions;
        std::optional<std::size_t> retainedActivityRecords;
        std::optional<std::size_t> evictedNotices;
        std::optional<std::size_t> evictedProcesses;
        std::optional<std::uint64_t> droppedProcessOutputBytes;
        std::optional<std::size_t> evictedFilesystemWatches;
        std::optional<std::size_t> evictedFuzzySearchSessions;
        std::optional<std::size_t> evictedActivityRecords;
        SafeDetail extensions;

        bool operator==(const CapacityState&) const = default;
    };

    struct BackendCursorMetadata {
        std::optional<std::uint64_t> backendRevision;
        std::optional<FrontendSequence> oldestReplayableAfter;
        std::optional<FrontendSequence> currentSequence;
        std::optional<FrontendSequence> oldestRetainedSequence;
        std::optional<FrontendSequence> newestRetainedSequence;
        std::optional<bool> backendSequenceExhausted;
        std::optional<bool> frontendSequenceExhausted;
        std::optional<SourceStamp> sourceStamp;

        bool operator==(const BackendCursorMetadata&) const = default;
    };

    struct LegacyRootExtension {
        std::string name;
        SafeDetail value;

        bool operator==(const LegacyRootExtension&) const = default;
    };

    struct CanonicalSnapshot {
        FrontendSequence sequence;
        ProviderState provider;
        ControllerState controller;
        std::vector<SessionState> sessions;
        bool sessionsPresent = true;
        ThreadListState threadList;
        bool threadListPresent = true;
        std::vector<ThreadState> threads;
        bool threadsPresent = true;
        std::vector<TurnState> turns;
        bool turnsPresent = true;
        std::vector<ThreadItem> items;
        bool itemsPresent = true;
        std::vector<LegacyItemCompatibility> legacyItems;
        std::vector<PendingRequest> pendingRequests;
        bool pendingRequestsPresent = true;
        std::vector<LegacyPendingRequestCompatibility> legacyPendingRequests;
        AccountsState accounts;
        ModelsState models;
        ConfigurationState configuration;
        std::vector<ProcessState> processes;
        DomainState processesState = DomainState::present();
        FilesystemWatchesState filesystemWatches;
        FuzzySearchesState fuzzySearches;
        PermissionProfilesState permissionProfiles;
        ReviewsState reviews;
        AppsState apps;
        ExternalAgentsState externalAgents;
        HooksState hooks;
        MarketplaceState marketplace;
        PluginsState plugins;
        SkillsState skills;
        McpState mcp;
        WindowsSandboxState windowsSandbox;
        PlatformState platform;
        RemoteControlState remoteControl;
        IntegrationsState integrations;
        NoticesState notices;
        ActivitiesState activities;
        DiagnosticsState diagnostics;
        CapacityState capacity;
        bool capacityPresent = true;
        TruncationMetadata truncation;
        ProjectionMetadata projection;
        BackendCursorMetadata backendCursor;
        SafeDetail stateExtensions;
        SafeDetail extensions;
        // Sanitized, bounded unknown members from a legacy snapshot's state
        // root.  They remain separate so ExpandedV1 state extensions keep
        // their established nested compatibility shape.
        std::vector<LegacyRootExtension> legacyRootExtensions;

        bool operator==(const CanonicalSnapshot&) const = default;
    };

    enum class ModelErrorCode { InvalidShape, InvalidIdentifier, UnsafeDetail, UnsupportedDiscriminator };

    struct ModelError {
        ModelErrorCode code = ModelErrorCode::InvalidShape;
        std::string path;
        std::string message;

        bool operator==(const ModelError&) const = default;
    };

    template <typename Value>
    class ModelResult {
    public:
        ModelResult(Value value)
            : result(std::move(value)) {
        }

        ModelResult(ModelError error)
            : result(std::move(error)) {
        }

        [[nodiscard]] bool hasValue() const noexcept {
            return std::holds_alternative<Value>(result);
        }

        explicit operator bool() const noexcept {
            return hasValue();
        }

        [[nodiscard]] const Value& value() const& {
            return std::get<Value>(result);
        }

        [[nodiscard]] Value&& value() && {
            return std::get<Value>(std::move(result));
        }

        [[nodiscard]] const ModelError& error() const& {
            return std::get<ModelError>(result);
        }

    private:
        std::variant<Value, ModelError> result;
    };

    [[nodiscard]] ModelResult<ExpandedSnapshot> encodeSnapshot(const CanonicalSnapshot& snapshot) noexcept;
    [[nodiscard]] ModelResult<CanonicalSnapshot> decodeSnapshot(const ExpandedSnapshot& snapshot) noexcept;
    [[nodiscard]] ModelResult<Snapshot> encodeLegacySnapshot(const CanonicalSnapshot& snapshot) noexcept;
    [[nodiscard]] ModelResult<CanonicalSnapshot> decodeLegacySnapshot(const Snapshot& snapshot) noexcept;

    struct SnapshotRepresentationSelection {
        bool expandedDomains = false;
        bool expandedItems = false;
        bool expandedPendingRequests = false;
        bool includeProjectionMetadata = false;

        bool operator==(const SnapshotRepresentationSelection&) const = default;
    };

    [[nodiscard]] SnapshotRepresentationSelection
    snapshotRepresentationSelection(std::span<const FrontendCapability> selectedCapabilities) noexcept;
    [[nodiscard]] ModelResult<Snapshot> encodeProjectedSnapshot(const CanonicalSnapshot& snapshot,
                                                                SnapshotRepresentationSelection selection) noexcept;
    [[nodiscard]] ModelResult<Snapshot> encodeProjectedSnapshot(const CanonicalSnapshot& snapshot,
                                                                std::span<const FrontendCapability> selectedCapabilities) noexcept;
    [[nodiscard]] ModelResult<CanonicalSnapshot> decodeProjectedSnapshot(const Snapshot& snapshot,
                                                                         SnapshotRepresentationSelection selection) noexcept;
    [[nodiscard]] ModelResult<CanonicalSnapshot> decodeProjectedSnapshot(const Snapshot& snapshot,
                                                                         std::span<const FrontendCapability> selectedCapabilities) noexcept;

    static_assert(std::variant_size_v<ThreadItem> == 18);
    static_assert(std::variant_size_v<PendingRequest> == 10);

} // namespace ai::openai::codex::frontend::internal::model

#endif // AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_MODEL_H
