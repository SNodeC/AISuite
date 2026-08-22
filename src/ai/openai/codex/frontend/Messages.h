/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_MESSAGES_H
#define AI_OPENAI_CODEX_FRONTEND_MESSAGES_H

#include "ai/openai/codex/frontend/Protocol.h"
#include "ai/openai/codex/frontend/Security.h"

#include <compare>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ai::openai::codex::frontend {

    using Json = nlohmann::json;

    class SequenceNumber {
    public:
        using Value = std::uint64_t;

        constexpr SequenceNumber() noexcept = default;

        explicit constexpr SequenceNumber(Value value) noexcept
            : sequence(value) {
        }

        [[nodiscard]] constexpr Value value() const noexcept {
            return sequence;
        }

        [[nodiscard]] static constexpr SequenceNumber maximum() noexcept {
            return SequenceNumber(std::numeric_limits<Value>::max());
        }

        auto operator<=>(const SequenceNumber&) const = default;

    private:
        Value sequence = 0;
    };

    enum class SessionRole { Observer, Controller };
    enum class SyncMode { Replay, Snapshot };
    enum class ThreadReadStateEffectAuthority { Merge, Replace, Absent };

    // Transport delivery has three distinct outcomes. Backpressured retains
    // the exact message in the bounded ServerCore queue for a later retry;
    // Closed is terminal for this frontend connection.
    enum class OutboundDeliveryStatus { Accepted, Backpressured, Closed };

    // These string values are part of Frontend Protocol v1 and must remain
    // stable. Command failures and frontend-local protocol failures share one
    // enum so applications can implement one exhaustive error policy.
    enum class ErrorCode {
        PermissionDenied,
        InvalidCommand,
        NotFound,
        Conflict,
        LocalSubmissionFailure,
        TypedDecodingFailure,
        RemoteAppServerError,
        Cancelled,
        BackendUnavailable,
        DuplicateRequestId,
        MalformedJson,
        WrongProtocol,
        UnsupportedVersion,
        MissingField,
        InvalidField,
        UnknownKind,
        UnknownMethod,
        FrameTooLarge,
        CapacityExceeded,
        SequenceOverflow,
        ReplayGap,
        InternalError,
        AuthenticationRequired,
        AuthenticationFailed,
        OriginRejected,
        TransportSecurityRequired,
        RateLimited
    };

    enum class FrontendCapability {
        MethodDiscovery,
        SecurityScopes,
        CompleteProviderOperations,
        CompleteReverseRequests,
        CompleteBackendDomains,
        ConditionalFilesystem,
        ConditionalCommandExecution,
        DedicatedPendingRequests,
        DedicatedNotificationEvents,
        CompleteThreadItems,
        ThreadReadStateEffects,
        AuthenticatedFrontend,
        ScopeProjectedState,
        ProviderLifecycle,
        MultiTransport,
        CppClientSdk,
        TypescriptClientSdk,
        BrowserUi,
        QtUi
    };

    enum class ThreadItemKind {
        AgentMessage,
        CollabAgentToolCall,
        CommandExecution,
        ContextCompaction,
        DynamicToolCall,
        EnteredReviewMode,
        ExitedReviewMode,
        FileChange,
        HookPrompt,
        ImageGeneration,
        ImageView,
        McpToolCall,
        Plan,
        Reasoning,
        Sleep,
        SubAgentActivity,
        UserMessage,
        WebSearch
    };

    enum class PendingRequestKind {
        CommandExecutionApproval,
        FileChangeApproval,
        UserInput,
        Authentication,
        ApplyPatchApproval,
        ExecCommandApproval,
        PermissionsApproval,
        Attestation,
        DynamicToolCall,
        McpElicitation
    };

    enum class ExpandedEventType {
        ProviderUpdated,
        ControllerUpdated,
        SessionsUpdated,
        ThreadListUpdated,
        ThreadUpserted,
        ThreadRemoved,
        TurnUpserted,
        ItemUpserted,
        ItemContentUpdated,
        PendingRequestsUpdated,
        AccountUpdated,
        ModelsUpdated,
        ConfigurationUpdated,
        ProcessUpdated,
        FilesystemWatchUpdated,
        FuzzySearchUpdated,
        ReviewsUpdated,
        IntegrationsUpdated,
        PluginsUpdated,
        SkillsUpdated,
        McpUpdated,
        PlatformUpdated,
        NoticeAdded,
        ActivityUpdated,
        CapacityUpdated,
        DiagnosticsUpdated
    };

    enum class StateFreshness { Unknown, Current, Stale };

    [[nodiscard]] std::string_view toString(SessionRole role) noexcept;
    [[nodiscard]] std::string_view toString(SyncMode mode) noexcept;
    [[nodiscard]] std::string_view toString(ErrorCode code) noexcept;
    [[nodiscard]] std::string_view toString(FrontendCapability capability) noexcept;
    [[nodiscard]] std::string_view toString(ThreadItemKind kind) noexcept;
    [[nodiscard]] std::string_view toString(PendingRequestKind kind) noexcept;
    [[nodiscard]] std::string_view toString(ExpandedEventType type) noexcept;
    [[nodiscard]] std::string_view toString(StateFreshness freshness) noexcept;
    [[nodiscard]] std::string_view toString(ThreadReadStateEffectAuthority authority) noexcept;
    [[nodiscard]] std::optional<SessionRole> sessionRoleFromString(std::string_view value) noexcept;
    [[nodiscard]] std::optional<SyncMode> syncModeFromString(std::string_view value) noexcept;
    [[nodiscard]] std::optional<ErrorCode> errorCodeFromString(std::string_view value) noexcept;
    [[nodiscard]] std::optional<FrontendCapability> frontendCapabilityFromString(std::string_view value) noexcept;
    [[nodiscard]] std::optional<ThreadItemKind> threadItemKindFromString(std::string_view value) noexcept;
    [[nodiscard]] std::optional<PendingRequestKind> pendingRequestKindFromString(std::string_view value) noexcept;
    [[nodiscard]] std::optional<ExpandedEventType> expandedEventTypeFromString(std::string_view value) noexcept;
    [[nodiscard]] std::optional<StateFreshness> stateFreshnessFromString(std::string_view value) noexcept;
    [[nodiscard]] std::optional<ThreadReadStateEffectAuthority>
    threadReadStateEffectAuthorityFromString(std::string_view value) noexcept;

    struct ThreadReadStateEffect {
        ThreadReadStateEffectAuthority authority = ThreadReadStateEffectAuthority::Merge;
        bool sourcePartial = false;
        bool responseTruncated = false;
        std::uint64_t responseOmittedTurns = 0;
        std::uint64_t responseOmittedItems = 0;

        bool operator==(const ThreadReadStateEffect&) const = default;
    };

    [[nodiscard]] constexpr bool threadReadStateEffectMetadataIsConsistent(
        const ThreadReadStateEffect& value) noexcept {
        const bool omitted = value.responseOmittedTurns != 0
                             || value.responseOmittedItems != 0;
        const bool structuralPartial = value.sourcePartial
                                       || value.responseTruncated;
        return value.responseTruncated == omitted
               && (value.authority == ThreadReadStateEffectAuthority::Merge)
                      == structuralPartial
               && !(value.authority == ThreadReadStateEffectAuthority::Absent
                    && structuralPartial);
    }

    [[nodiscard]] constexpr bool threadReadStateEffectAuthorityMatchesPayload(
        ThreadReadStateEffectAuthority authority, std::optional<bool> threadFullyLoaded) noexcept {
        switch (authority) {
            case ThreadReadStateEffectAuthority::Merge:
                return threadFullyLoaded.has_value() && !*threadFullyLoaded;
            case ThreadReadStateEffectAuthority::Replace:
                return threadFullyLoaded.has_value() && *threadFullyLoaded;
            case ThreadReadStateEffectAuthority::Absent:
                return !threadFullyLoaded.has_value();
        }
        return false;
    }

    [[nodiscard]] std::optional<Json> encodeThreadReadStateEffect(const ThreadReadStateEffect& value) noexcept;
    [[nodiscard]] std::optional<ThreadReadStateEffect> decodeThreadReadStateEffect(const Json& value, std::string& error) noexcept;

    using FrontendMethod = std::string;

    struct CapabilityAdvertisement {
        std::vector<FrontendCapability> defined;
        std::vector<FrontendCapability> implemented;
        std::vector<FrontendCapability> permitted;
        Json extensions = Json::object();

        bool operator==(const CapabilityAdvertisement&) const = default;
    };

    struct ExpandedThreadItem {
        std::string id;
        ThreadItemKind type = ThreadItemKind::AgentMessage;
        std::optional<std::string> threadId;
        std::optional<std::string> turnId;
        std::optional<std::string> status;
        std::optional<std::string> summary;
        std::optional<Json> location;
        std::optional<std::string> agentText;
        std::optional<std::string> reasoningText;
        std::optional<std::string> reasoningSummary;
        std::optional<std::string> commandOutput;
        std::optional<std::uint64_t> droppedContentBytes;
        std::optional<bool> contentTruncated;
        std::optional<std::int64_t> startedAtMs;
        std::optional<std::int64_t> completedAtMs;
        std::optional<Json> data;
        bool truncated = false;
        std::vector<std::string> omittedFields;
        bool connectionInvalidated = false;
        std::optional<std::uint64_t> generation;
        std::optional<StateFreshness> freshness;
        Json extensions = Json::object();

        bool operator==(const ExpandedThreadItem&) const = default;
    };

    struct ExpandedPendingRequestOption {
        std::string label;
        std::string description;
        Json extensions = Json::object();

        bool operator==(const ExpandedPendingRequestOption&) const = default;
    };

    struct ExpandedPendingRequestQuestion {
        std::string id;
        std::string header;
        std::string prompt;
        bool allowsFreeText = false;
        // This is presentation metadata only. Secret answers are never part of
        // a pending-request projection.
        bool isSecret = false;
        std::vector<ExpandedPendingRequestOption> options;
        Json extensions = Json::object();

        bool operator==(const ExpandedPendingRequestQuestion&) const = default;
    };

    struct ExpandedPendingRequest {
        std::string pendingRequestId;
        PendingRequestKind kind = PendingRequestKind::CommandExecutionApproval;
        std::optional<std::string> threadId;
        std::optional<std::string> turnId;
        std::optional<std::string> itemId;
        std::optional<std::string> summary;
        std::optional<Json> details;
        std::optional<std::vector<ExpandedPendingRequestQuestion>> questions;
        std::optional<std::uint64_t> autoResolutionMs;
        bool truncated = false;
        Json extensions = Json::object();

        bool operator==(const ExpandedPendingRequest&) const = default;
    };

    struct ExpandedFrontendEvent {
        SequenceNumber sequence;
        ExpandedEventType type = ExpandedEventType::ProviderUpdated;
        Json data = Json::object();
        Json extensions = Json::object();

        bool operator==(const ExpandedFrontendEvent&) const = default;
    };

    struct ExpandedBackendSnapshotState {
        Json provider = Json::object();
        Json controller = Json::object();
        std::vector<Json> sessions;
        Json threadList = Json::object();
        std::optional<std::vector<Json>> threads;
        std::optional<std::vector<Json>> turns;
        std::optional<std::vector<ExpandedThreadItem>> items;
        std::optional<std::vector<ExpandedPendingRequest>> pendingRequests;
        std::optional<Json> accounts;
        std::optional<Json> models;
        std::optional<Json> configuration;
        std::optional<Json> processes;
        std::optional<Json> filesystemWatches;
        std::optional<Json> fuzzySearches;
        std::optional<Json> permissionProfiles;
        std::optional<Json> reviews;
        std::optional<Json> apps;
        std::optional<Json> externalAgents;
        std::optional<Json> hooks;
        std::optional<Json> marketplace;
        std::optional<Json> plugins;
        std::optional<Json> skills;
        std::optional<Json> mcp;
        std::optional<Json> windowsSandbox;
        std::optional<Json> remoteControl;
        std::optional<Json> notices;
        std::optional<Json> activities;
        Json capacity = Json::object();
        Json truncation = Json::object();
        Json extensions = Json::object();

        bool operator==(const ExpandedBackendSnapshotState&) const = default;
    };

    struct ExpandedSnapshot {
        SequenceNumber sequence;
        ExpandedBackendSnapshotState state;
        Json extensions = Json::object();

        bool operator==(const ExpandedSnapshot&) const = default;
    };

    struct Hello {
        std::optional<SequenceNumber> resumeAfter;
        Json extensions = Json::object();
        std::optional<std::vector<FrontendCapability>> capabilities;
        std::optional<AuthenticationCredential> authentication;
        std::optional<std::uint32_t> capabilityVocabularyVersion = CapabilityVocabularyVersion;

        Hello() = default;

        Hello(std::optional<SequenceNumber> resumeAfter,
              Json extensions = Json::object(),
              std::optional<std::vector<FrontendCapability>> capabilities = std::nullopt,
              std::optional<AuthenticationCredential> authentication = std::nullopt,
              std::optional<std::uint32_t> capabilityVocabularyVersion = CapabilityVocabularyVersion)
            : resumeAfter(resumeAfter)
            , extensions(std::move(extensions))
            , capabilities(std::move(capabilities))
            , authentication(std::move(authentication))
            , capabilityVocabularyVersion(capabilityVocabularyVersion) {
        }

        bool operator==(const Hello&) const = default;
    };

    struct ControllerAcquire {
        bool operator==(const ControllerAcquire&) const = default;
    };
    struct ControllerRelease {
        bool operator==(const ControllerRelease&) const = default;
    };
    struct SnapshotGet {
        bool operator==(const SnapshotGet&) const = default;
    };

    struct ReplayAfter {
        SequenceNumber after;

        bool operator==(const ReplayAfter&) const = default;
    };

    struct ThreadStart {
        std::optional<std::string> cwd;
        std::optional<std::string> model;
        std::optional<std::string> modelProvider;
        std::optional<std::string> approvalPolicy;
        std::optional<std::string> sandboxMode;
        std::optional<bool> ephemeral;

        bool operator==(const ThreadStart&) const = default;
    };

    struct ThreadResume {
        std::string threadId;
        std::optional<std::string> cwd;
        std::optional<std::string> model;
        std::optional<std::string> modelProvider;
        std::optional<std::string> approvalPolicy;
        std::optional<std::string> sandboxMode;

        bool operator==(const ThreadResume&) const = default;
    };

    struct ThreadList {
        std::optional<std::string> cursor;
        std::optional<std::uint32_t> limit;
        std::optional<bool> archived;
        std::optional<std::string> searchTerm;

        bool operator==(const ThreadList&) const = default;
    };

    struct ThreadRead {
        std::string threadId;
        std::optional<bool> includeTurns;

        bool operator==(const ThreadRead&) const = default;
    };

    struct TextInput {
        std::string text;
        Json extensions = Json::object();

        bool operator==(const TextInput&) const = default;
    };

    struct ImageUrlInput {
        std::string url;
        std::optional<std::string> detail;
        Json extensions = Json::object();

        bool operator==(const ImageUrlInput&) const = default;
    };

    struct LocalImageInput {
        std::string path;
        std::optional<std::string> detail;
        Json extensions = Json::object();

        bool operator==(const LocalImageInput&) const = default;
    };

    struct SkillInput {
        std::string name;
        std::string path;
        Json extensions = Json::object();

        bool operator==(const SkillInput&) const = default;
    };

    struct MentionInput {
        std::string name;
        std::string path;
        Json extensions = Json::object();

        bool operator==(const MentionInput&) const = default;
    };

    using TurnInput = std::variant<TextInput, ImageUrlInput, LocalImageInput, SkillInput, MentionInput>;

    using SandboxNetworkAccess = std::variant<bool, std::string>;

    struct SandboxPolicy {
        std::string type;
        std::optional<SandboxNetworkAccess> networkAccess;
        std::vector<std::string> writableRoots;
        std::optional<bool> excludeTmpdirEnvVar;
        std::optional<bool> excludeSlashTmp;
        Json extensions = Json::object();

        bool operator==(const SandboxPolicy&) const = default;
    };

    struct TurnStart {
        std::string threadId;
        std::vector<TurnInput> input;
        std::optional<std::string> cwd;
        std::optional<std::string> model;
        std::optional<std::string> reasoningEffort;
        std::optional<std::string> approvalPolicy;
        std::optional<SandboxPolicy> sandboxPolicy;

        bool operator==(const TurnStart&) const = default;
    };

    struct TurnInterrupt {
        std::string threadId;
        std::string turnId;

        bool operator==(const TurnInterrupt&) const = default;
    };

    struct ApprovalRespond {
        std::string pendingRequestId;
        std::string decision;

        bool operator==(const ApprovalRespond&) const = default;
    };

    struct UserInputAnswer {
        std::string questionId;
        std::vector<std::string> answers;

        bool operator==(const UserInputAnswer&) const = default;
    };

    struct UserInputRespond {
        std::string pendingRequestId;
        std::vector<UserInputAnswer> answers;

        bool operator==(const UserInputRespond&) const = default;
    };

    struct AuthenticationRespond {
        std::string pendingRequestId;
        std::string accessToken;
        std::string chatgptAccountId;
        std::optional<std::string> chatgptPlanType;

        bool operator==(const AuthenticationRespond&) const = default;
    };

    struct UnknownRequestRespond {
        std::string pendingRequestId;
        Json result = nullptr;

        bool operator==(const UnknownRequestRespond&) const = default;
    };

    struct UnknownRequestReject {
        std::string pendingRequestId;
        std::int64_t code = 0;
        std::string message;
        std::optional<Json> data;

        bool operator==(const UnknownRequestReject&) const = default;
    };

    enum class CommandMethod {
        ControllerAcquire,
        ControllerRelease,
        SnapshotGet,
        EventsReplay,
        ThreadStart,
        ThreadResume,
        ThreadList,
        ThreadRead,
        TurnStart,
        TurnInterrupt,
        ApprovalRespond,
        UserInputRespond,
        AuthenticationRespond,
        UnknownRequestRespond,
        UnknownRequestReject
    };

    using CommandParameters = std::variant<ControllerAcquire,
                                           ControllerRelease,
                                           SnapshotGet,
                                           ReplayAfter,
                                           ThreadStart,
                                           ThreadResume,
                                           ThreadList,
                                           ThreadRead,
                                           TurnStart,
                                           TurnInterrupt,
                                           ApprovalRespond,
                                           UserInputRespond,
                                           AuthenticationRespond,
                                           UnknownRequestRespond,
                                           UnknownRequestReject>;

    [[nodiscard]] std::string_view toString(CommandMethod method) noexcept;
    [[nodiscard]] std::optional<CommandMethod> commandMethodFromString(std::string_view value) noexcept;
    [[nodiscard]] CommandMethod commandMethod(const CommandParameters& parameters) noexcept;

    struct Command {
        std::string requestId;
        CommandParameters parameters;
        Json extensions = Json::object();
        Json parameterExtensions = Json::object();

        bool operator==(const Command&) const = default;
    };

    using ClientMessage = std::variant<Hello, Command>;

    struct Welcome {
        std::string sessionId;
        SessionRole role = SessionRole::Observer;
        SequenceNumber currentSequence;
        SyncMode syncMode = SyncMode::Snapshot;
        Json extensions = Json::object();
        std::optional<CapabilityAdvertisement> capabilities;
        std::optional<std::vector<FrontendMethod>> availableMethods;
        std::optional<std::vector<FrontendMethod>> permittedMethods;
        std::optional<std::string> serverVersion;
        std::optional<std::uint64_t> maximumInboundMessageBytes;

        Welcome() = default;

        Welcome(std::string sessionId,
                SessionRole role = SessionRole::Observer,
                SequenceNumber currentSequence = {},
                SyncMode syncMode = SyncMode::Snapshot,
                Json extensions = Json::object(),
                std::optional<CapabilityAdvertisement> capabilities = std::nullopt,
                std::optional<std::vector<FrontendMethod>> availableMethods = std::nullopt,
                std::optional<std::vector<FrontendMethod>> permittedMethods = std::nullopt,
                std::optional<std::string> serverVersion = std::nullopt,
                std::optional<std::uint64_t> maximumInboundMessageBytes = std::nullopt)
            : sessionId(std::move(sessionId))
            , role(role)
            , currentSequence(currentSequence)
            , syncMode(syncMode)
            , extensions(std::move(extensions))
            , capabilities(std::move(capabilities))
            , availableMethods(std::move(availableMethods))
            , permittedMethods(std::move(permittedMethods))
            , serverVersion(std::move(serverVersion))
            , maximumInboundMessageBytes(maximumInboundMessageBytes) {
        }

        bool operator==(const Welcome&) const = default;
    };

    struct SyncComplete {
        SequenceNumber sequence;
        Json extensions = Json::object();

        bool operator==(const SyncComplete&) const = default;
    };

    struct Snapshot {
        SequenceNumber sequence;
        Json state = Json::object();
        Json extensions = Json::object();

        bool operator==(const Snapshot&) const = default;
    };

    struct FrontendEvent {
        SequenceNumber sequence;
        std::string type;
        Json data = Json::object();
        Json extensions = Json::object();

        bool operator==(const FrontendEvent&) const = default;
    };

    struct EventBatch {
        SequenceNumber fromSequence;
        SequenceNumber toSequence;
        std::vector<FrontendEvent> events;
        Json extensions = Json::object();

        bool operator==(const EventBatch&) const = default;
    };

    struct CommandError {
        ErrorCode code = ErrorCode::InternalError;
        std::string message;
        std::optional<Json> details;
        Json extensions = Json::object();

        bool operator==(const CommandError&) const = default;
    };

    struct Response {
        std::string requestId;
        bool ok = false;
        std::optional<Json> result;
        std::optional<CommandError> error;
        Json extensions = Json::object();

        [[nodiscard]] static Response success(std::string requestId, Json result = Json::object());
        [[nodiscard]] static Response failure(std::string requestId, CommandError error);

        bool operator==(const Response&) const = default;
    };

    struct ProtocolErrorMessage {
        ErrorCode code = ErrorCode::InvalidCommand;
        std::string message;
        std::vector<std::uint32_t> supportedVersions;
        bool closeConnection = false;
        std::optional<std::string> requestId;
        std::optional<Json> details;
        Json extensions = Json::object();

        bool operator==(const ProtocolErrorMessage&) const = default;
    };

    using ServerMessage = std::variant<Welcome, SyncComplete, Snapshot, EventBatch, Response, ProtocolErrorMessage>;

} // namespace ai::openai::codex::frontend

#endif // AI_OPENAI_CODEX_FRONTEND_MESSAGES_H
