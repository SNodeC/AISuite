#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_STATETYPES_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_STATETYPES_H

#include "ai/openai/codex/frontend/Messages.h"
#include "ai/openai/codex/typed/Accounts.h"
#include "ai/openai/codex/typed/Configuration.h"
#include "ai/openai/codex/typed/Events.h"
#include "ai/openai/codex/typed/Filesystem.h"
#include "ai/openai/codex/typed/Mcp.h"
#include "ai/openai/codex/typed/Types.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ai::openai::codex::frontend::client {

    // Additive fields in the stable expanded-state schemas are constrained to
    // Frontend Protocol SafeDetailValue values.  The Codec validates the
    // per-object schema bounds before reduction, and maximumDecodedStateBytes
    // bounds their aggregate retention in an immutable State.
    using BoundedSafeDetailExtensions = frontend::Json;

    template <typename T>
    struct Projected {
        std::optional<T> value;
        bool truncated = false;
        std::vector<std::string> omittedFields;

        bool operator==(const Projected&) const = default;
    };

    struct FrontendSessionId {
        std::string value;
        auto operator<=>(const FrontendSessionId&) const = default;
    };

    struct PendingRequestId {
        std::string value;
        auto operator<=>(const PendingRequestId&) const = default;
    };

    struct ProcessHandle {
        std::string value;
        auto operator<=>(const ProcessHandle&) const = default;
    };

    struct FuzzySearchSessionId {
        std::string value;
        auto operator<=>(const FuzzySearchSessionId&) const = default;
    };

    struct ActivityKey {
        std::string value;
        auto operator<=>(const ActivityKey&) const = default;
    };

    struct SourceStamp {
        std::uint64_t generation = 0;
        frontend::StateFreshness freshness = frontend::StateFreshness::Unknown;
        BoundedSafeDetailExtensions extensions = frontend::Json::object();

        bool operator==(const SourceStamp&) const = default;
    };

    struct TruncationMetadata {
        bool truncated = false;
        std::vector<std::string> omittedFields;
        std::optional<std::size_t> omittedEntries;
        std::optional<std::uint64_t> droppedBytes;
        BoundedSafeDetailExtensions extensions = frontend::Json::object();

        bool operator==(const TruncationMetadata&) const = default;
    };

    // Cursor facts that exist in the legacy v1 snapshot are typed rather
    // than being mislabeled as forward-compatible JSON. Expanded snapshots
    // populate only the facts they actually carry.
    struct BackendCursorState {
        std::optional<std::uint64_t> backendRevision;
        std::optional<frontend::SequenceNumber> oldestReplayableAfter;
        std::optional<frontend::SequenceNumber> currentSequence;
        std::optional<frontend::SequenceNumber> oldestRetainedSequence;
        std::optional<frontend::SequenceNumber> newestRetainedSequence;
        std::optional<bool> backendSequenceExhausted;
        std::optional<bool> frontendSequenceExhausted;

        bool operator==(const BackendCursorState&) const = default;
    };

    // Exact, language-independent paths reported by the server's mandatory
    // scope projection.  Redaction is distinct from omission: redacted values
    // remain present as bounded placeholders, while omitted values are absent
    // from the principal's projection.
    struct ProjectionMetadataState {
        std::vector<std::string> omittedFields;
        std::vector<std::string> redactedFields;

        bool operator==(const ProjectionMetadataState&) const = default;
    };

    // Canonical language-independent serialization of the projection inputs
    // associated with this immutable State.  It contains only the non-secret
    // continuity key supplied by the application and server-advertised
    // projection facts; authentication credentials are never included.
    struct ProjectionFingerprintMetadata {
        std::string canonical;

        bool operator==(const ProjectionFingerprintMetadata&) const = default;
    };

    enum class ProviderLifecycle { Stopped, Starting, Initializing, Ready, Stopping, Failed, Recovering };
    enum class ProviderRecoveryStatus { Idle, Waiting, Exhausted };

    struct ProviderRecoveryState {
        ProviderRecoveryStatus status = ProviderRecoveryStatus::Idle;
        std::size_t attempts = 0;
        std::optional<std::uint64_t> delayMs;
        BoundedSafeDetailExtensions extensions = frontend::Json::object();

        bool operator==(const ProviderRecoveryState&) const = default;
    };

    struct ProviderErrorState {
        std::string category;
        std::int64_t code = 0;
        std::optional<bool> detailsOmitted;
        std::optional<std::string> message;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const ProviderErrorState&) const = default;
    };

    struct ProviderInitializationState {
        typed::AbsolutePath codexHome;
        std::string platformFamily;
        std::string platformOs;
        std::string userAgent;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const ProviderInitializationState&) const = default;
    };

    struct ProviderState {
        ProviderLifecycle lifecycle = ProviderLifecycle::Stopped;
        std::uint64_t generation = 0;
        bool desiredRunning = false;
        ProviderRecoveryState recovery;
        std::optional<ProviderErrorState> lastError;
        std::optional<ProviderInitializationState> initialization;
        bool ready = false;
        BoundedSafeDetailExtensions extensions = frontend::Json::object();

        bool operator==(const ProviderState&) const = default;
    };

    struct ControllerState {
        std::optional<FrontendSessionId> sessionId;
        bool present = false;
        bool ownedByThisClient = false;
        BoundedSafeDetailExtensions extensions = frontend::Json::object();

        bool operator==(const ControllerState&) const = default;
    };

    struct SessionState {
        FrontendSessionId sessionId;
        frontend::SessionRole role = frontend::SessionRole::Observer;
        BoundedSafeDetailExtensions extensions = frontend::Json::object();

        bool operator==(const SessionState&) const = default;
    };

    struct ThreadListState {
        bool hasLoadedPage = false;
        bool complete = false;
        std::size_t pagesLoaded = 0;
        std::optional<std::string> nextCursor;
        std::optional<std::string> backwardsCursor;
        std::optional<SourceStamp> stamp;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const ThreadListState&) const = default;
    };

    struct ThreadRealtimeState {
        std::string lifecycle;
        std::string transcript;
        std::size_t itemCount = 0;
        std::uint64_t receivedAudioBytes = 0;
        std::uint64_t droppedAudioBytes = 0;
        bool transcriptTruncated = false;
        std::optional<bool> errorDetailsOmitted;
        std::optional<std::string> sessionId;
        std::optional<typed::RealtimeConversationVersion> version;
        std::optional<std::uint64_t> lastSdpBytes;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const ThreadRealtimeState&) const = default;
    };

    struct ThreadState {
        typed::ThreadId id;
        std::optional<std::string> title;
        std::optional<std::string> preview;
        std::optional<typed::AbsolutePath> cwd;
        std::optional<typed::ModelId> model;
        std::optional<std::string> modelProvider;
        std::optional<std::string> status;
        bool fullyLoaded = false;
        std::optional<ThreadRealtimeState> realtime;
        std::optional<SourceStamp> stamp;
        std::optional<std::int64_t> createdAtMs;
        std::optional<std::int64_t> updatedAtMs;
        std::vector<typed::TurnId> orderedTurns;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const ThreadState&) const = default;
    };

    struct TurnState {
        typed::TurnId id;
        typed::ThreadId threadId;
        typed::TurnStatus status;
        bool active = false;
        bool terminal = false;
        bool connectionInvalidated = false;
        std::optional<SourceStamp> stamp;
        std::vector<typed::ItemId> orderedItems;
        // ProtocolDefinedOpaqueJson: Frontend Protocol v1 deliberately
        // defines both values as bounded SafeDetailObject projections rather
        // than as the provider's richer private wire types.
        std::optional<frontend::Json> failure;
        std::optional<frontend::Json> tokenUsage;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const TurnState&) const = default;
    };

    struct ItemKind {
        std::string identity = std::string(frontend::toString(frontend::ThreadItemKind::AgentMessage));
        std::optional<frontend::ThreadItemKind> known = frontend::ThreadItemKind::AgentMessage;

        ItemKind() = default;
        ItemKind(frontend::ThreadItemKind value)
            : identity(frontend::toString(value))
            , known(value) {
        }
        ItemKind(std::string value, std::optional<frontend::ThreadItemKind> knownValue)
            : identity(std::move(value))
            , known(knownValue) {
        }

        [[nodiscard]] bool is(frontend::ThreadItemKind value) const noexcept {
            return known == value;
        }

        bool operator==(const ItemKind&) const = default;
    };

    enum class ItemContentChannel {
        AgentText,
        ReasoningText,
        ReasoningSummary,
        CommandOutput,
    };

    [[nodiscard]] constexpr std::string_view toString(ItemContentChannel channel) noexcept {
        switch (channel) {
            case ItemContentChannel::AgentText:
                return "agentText";
            case ItemContentChannel::ReasoningText:
                return "reasoningText";
            case ItemContentChannel::ReasoningSummary:
                return "reasoningSummary";
            case ItemContentChannel::CommandOutput:
                return "commandOutput";
        }
        return {};
    }

    struct ItemState {
        typed::ItemId id;
        std::optional<typed::ThreadId> threadId;
        std::optional<typed::TurnId> turnId;
        ItemKind kind;
        std::optional<std::string> status;
        std::optional<std::string> summary;
        // ProtocolDefinedOpaqueJson: the stable item shell is typed, while
        // location/data are explicitly bounded opaque projection positions.
        std::optional<frontend::Json> location;
        std::optional<std::string> agentText;
        std::optional<std::string> reasoningText;
        std::optional<std::string> reasoningSummary;
        std::optional<std::string> commandOutput;
        std::optional<std::uint64_t> droppedContentBytes;
        bool contentTruncated = false;
        std::optional<std::int64_t> startedAtMs;
        std::optional<std::int64_t> completedAtMs;
        std::optional<frontend::Json> data;
        bool truncated = false;
        std::vector<std::string> omittedFields;
        bool connectionInvalidated = false;
        std::optional<SourceStamp> stamp;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const ItemState&) const = default;
    };

    struct PendingRequestOptionState {
        std::string label;
        std::string description;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const PendingRequestOptionState&) const = default;
    };

    struct PendingRequestQuestionState {
        std::string id;
        std::string header;
        std::string prompt;
        bool allowsFreeText = false;
        bool isSecret = false;
        std::vector<PendingRequestOptionState> options;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const PendingRequestQuestionState&) const = default;
    };

    struct PendingRequestState {
        PendingRequestId id;
        frontend::PendingRequestKind kind = frontend::PendingRequestKind::CommandExecutionApproval;
        std::optional<typed::ThreadId> threadId;
        std::optional<typed::TurnId> turnId;
        std::optional<typed::ItemId> itemId;
        std::optional<std::string> summary;
        std::optional<frontend::Json> opaqueDetails;
        std::optional<std::vector<PendingRequestQuestionState>> questions;
        std::optional<std::uint64_t> autoResolutionMs;
        bool truncated = false;
        std::vector<std::string> omittedFields;
        bool connectionInvalidated = false;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const PendingRequestState&) const = default;
    };

    struct DomainResultSummaryState {
        std::string method;
        std::string status;
        std::optional<std::string> subjectId;
        std::optional<std::string> nextCursor;
        std::optional<std::size_t> itemCount;
        std::optional<bool> complete;
        SourceStamp stamp;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const DomainResultSummaryState&) const = default;
    };

    struct DomainProjectionState {
        std::optional<SourceStamp> stamp;
        std::optional<std::string> status;
        std::optional<std::string> summary;
        std::optional<std::string> nextCursor;
        std::optional<bool> complete;
        std::optional<std::size_t> itemCount;
        std::vector<DomainResultSummaryState> latestResults;
        std::optional<std::size_t> notificationCount;
        std::vector<std::string> latestNotificationMethods;
        frontend::Json opaqueDetails = frontend::Json::object();
        std::optional<TruncationMetadata> truncation;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const DomainProjectionState&) const = default;
    };

#define AISUITE_FRONTEND_CLIENT_SIMPLE_DOMAIN_TYPE(typeName)                                                                               \
    struct typeName {                                                                                                                       \
        DomainProjectionState projection;                                                                                                  \
        bool operator==(const typeName&) const = default;                                                                                   \
    }

    struct AccountDetailsState {
        std::optional<bool> loggedOut;
        std::optional<std::string> loginLifecycle;
        std::optional<std::string> loginMethod;
        std::optional<bool> loginSucceeded;
        std::optional<bool> authenticated;
        std::optional<std::string> accountType;
        std::optional<typed::AuthMode> authMode;
        std::optional<typed::PlanType> planType;
        std::optional<double> primaryUsedPercent;
        std::optional<double> secondaryUsedPercent;
        std::optional<bool> hasCredits;
        bool operator==(const AccountDetailsState&) const = default;
    };

    struct ConfigurationDetailsState {
        std::optional<typed::AbsolutePath> filePath;
        std::optional<typed::WriteStatus> writeStatus;
        std::optional<std::string> writeVersion;
        std::optional<bool> writeOverridden;
        std::optional<std::size_t> featureCount;
        std::optional<bool> featureListTruncated;
        bool operator==(const ConfigurationDetailsState&) const = default;
    };

    struct IntegrationDetailsState {
        std::optional<std::size_t> appCount;
        std::optional<bool> appListTruncated;
        std::optional<std::string> marketplaceAddStatus;
        std::optional<std::string> marketplaceRemoveStatus;
        std::optional<std::string> marketplaceUpgradeStatus;
        bool operator==(const IntegrationDetailsState&) const = default;
    };

    struct PluginsAndSkillsDetailsState {
        std::optional<std::string> lastPluginOperation;
        std::optional<std::string> lastSkillsOperation;
        std::optional<std::size_t> extraRootCount;
        std::optional<bool> extraRootsTruncated;
        bool operator==(const PluginsAndSkillsDetailsState&) const = default;
    };

    struct McpDetailsState {
        std::optional<std::string> oauthStatus;
        std::optional<typed::McpServerStartupState> startupStatus;
        std::optional<std::size_t> serverCount;
        std::optional<bool> statusListComplete;
        bool operator==(const McpDetailsState&) const = default;
    };

    struct PlatformDetailsState {
        std::optional<typed::RemoteControlConnectionStatus> remoteControlStatus;
        std::optional<std::string> windowsSandboxStatus;
        bool operator==(const PlatformDetailsState&) const = default;
    };

    struct AccountState {
        DomainProjectionState projection;
        AccountDetailsState details;
        bool operator==(const AccountState&) const = default;
    };
    AISUITE_FRONTEND_CLIENT_SIMPLE_DOMAIN_TYPE(ModelsState);
    struct ConfigurationState {
        DomainProjectionState projection;
        ConfigurationDetailsState details;
        bool operator==(const ConfigurationState&) const = default;
    };
    AISUITE_FRONTEND_CLIENT_SIMPLE_DOMAIN_TYPE(PermissionProfilesState);
    AISUITE_FRONTEND_CLIENT_SIMPLE_DOMAIN_TYPE(ReviewsState);
#define AISUITE_FRONTEND_CLIENT_INTEGRATION_DOMAIN_TYPE(typeName)                                                                          \
    struct typeName {                                                                                                                       \
        DomainProjectionState projection;                                                                                                  \
        IntegrationDetailsState details;                                                                                                   \
        bool operator==(const typeName&) const = default;                                                                                   \
    }
    AISUITE_FRONTEND_CLIENT_INTEGRATION_DOMAIN_TYPE(AppsState);
    AISUITE_FRONTEND_CLIENT_INTEGRATION_DOMAIN_TYPE(ExternalAgentsState);
    AISUITE_FRONTEND_CLIENT_INTEGRATION_DOMAIN_TYPE(HooksState);
    AISUITE_FRONTEND_CLIENT_INTEGRATION_DOMAIN_TYPE(MarketplaceState);
#undef AISUITE_FRONTEND_CLIENT_INTEGRATION_DOMAIN_TYPE
#define AISUITE_FRONTEND_CLIENT_PLUGIN_DOMAIN_TYPE(typeName)                                                                               \
    struct typeName {                                                                                                                       \
        DomainProjectionState projection;                                                                                                  \
        PluginsAndSkillsDetailsState details;                                                                                              \
        bool operator==(const typeName&) const = default;                                                                                   \
    }
    AISUITE_FRONTEND_CLIENT_PLUGIN_DOMAIN_TYPE(PluginsState);
    AISUITE_FRONTEND_CLIENT_PLUGIN_DOMAIN_TYPE(SkillsState);
#undef AISUITE_FRONTEND_CLIENT_PLUGIN_DOMAIN_TYPE
    struct McpState {
        DomainProjectionState projection;
        McpDetailsState details;
        bool operator==(const McpState&) const = default;
    };
#define AISUITE_FRONTEND_CLIENT_PLATFORM_DOMAIN_TYPE(typeName)                                                                             \
    struct typeName {                                                                                                                       \
        DomainProjectionState projection;                                                                                                  \
        PlatformDetailsState details;                                                                                                      \
        bool operator==(const typeName&) const = default;                                                                                   \
    }
    AISUITE_FRONTEND_CLIENT_PLATFORM_DOMAIN_TYPE(WindowsSandboxState);
    AISUITE_FRONTEND_CLIENT_PLATFORM_DOMAIN_TYPE(PlatformState);
#undef AISUITE_FRONTEND_CLIENT_PLATFORM_DOMAIN_TYPE
#undef AISUITE_FRONTEND_CLIENT_SIMPLE_DOMAIN_TYPE

    struct ProcessState {
        ProcessHandle processHandle;
        std::string lifecycle;
        std::optional<std::string> standardOutput;
        std::optional<std::string> standardError;
        std::optional<std::size_t> stdoutBytes;
        std::optional<std::size_t> stderrBytes;
        bool stdoutTruncated = false;
        bool stderrTruncated = false;
        std::optional<std::uint64_t> droppedOutputBytes;
        std::optional<std::int64_t> exitCode;
        SourceStamp stamp;
        bool connectionInvalidated = false;
        bool stateUnavailable = false;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const ProcessState&) const = default;
    };

    struct ProcessCollectionState {
        std::vector<ProcessState> entries;
        TruncationMetadata truncation;
        BoundedSafeDetailExtensions extensions = frontend::Json::object();
        bool operator==(const ProcessCollectionState&) const = default;
    };

    struct FilesystemWatchState {
        typed::FsWatchId watchId;
        std::optional<typed::AbsolutePath> root;
        std::optional<std::size_t> changedPathCount;
        SourceStamp stamp;
        bool connectionInvalidated = false;
        bool stateUnavailable = false;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const FilesystemWatchState&) const = default;
    };

    struct FilesystemWatchCollectionState {
        std::vector<FilesystemWatchState> entries;
        TruncationMetadata truncation;
        BoundedSafeDetailExtensions extensions = frontend::Json::object();
        bool operator==(const FilesystemWatchCollectionState&) const = default;
    };

    struct FuzzySearchState {
        FuzzySearchSessionId sessionId;
        std::optional<std::size_t> resultCount;
        bool complete = false;
        SourceStamp stamp;
        bool connectionInvalidated = false;
        bool stateUnavailable = false;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const FuzzySearchState&) const = default;
    };

    struct FuzzySearchCollectionState {
        std::vector<FuzzySearchState> entries;
        TruncationMetadata truncation;
        BoundedSafeDetailExtensions extensions = frontend::Json::object();
        bool operator==(const FuzzySearchCollectionState&) const = default;
    };

    struct NoticeState {
        std::optional<std::uint64_t> occurrence;
        std::string category;
        std::string summary;
        std::optional<std::string> details;
        std::optional<typed::ThreadId> threadId;
        SourceStamp stamp;
        bool stateUnavailable = false;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const NoticeState&) const = default;
    };

    struct NoticeCollectionState {
        std::vector<NoticeState> entries;
        TruncationMetadata truncation;
        BoundedSafeDetailExtensions extensions = frontend::Json::object();
        bool operator==(const NoticeCollectionState&) const = default;
    };

    struct ActivityState {
        ActivityKey key;
        std::optional<std::string> subjectId;
        std::string kind;
        std::string lifecycle;
        std::optional<std::string> summary;
        std::optional<std::string> details;
        std::optional<typed::ThreadId> threadId;
        std::optional<typed::TurnId> turnId;
        bool active = false;
        SourceStamp stamp;
        bool stateUnavailable = false;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const ActivityState&) const = default;
    };

    struct ActivityCollectionState {
        std::vector<ActivityState> entries;
        TruncationMetadata truncation;
        BoundedSafeDetailExtensions extensions = frontend::Json::object();
        bool operator==(const ActivityCollectionState&) const = default;
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
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const CapacityState&) const = default;
    };

    struct DiagnosticState {
        std::optional<std::uint64_t> received;
        bool detailsOmitted = false;
        std::optional<std::string> message;
        std::optional<frontend::Json> opaqueDetails;
        frontend::Json extensions = frontend::Json::object();

        bool operator==(const DiagnosticState&) const = default;
    };

    struct DiagnosticCollectionState {
        std::optional<std::uint64_t> received;
        std::vector<DiagnosticState> entries;

        bool operator==(const DiagnosticCollectionState&) const = default;
    };

} // namespace ai::openai::codex::frontend::client

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_STATETYPES_H
