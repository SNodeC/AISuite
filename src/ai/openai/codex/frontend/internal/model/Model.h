/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_MODEL_H
#define AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_MODEL_H

#include "ai/openai/codex/frontend/Messages.h"
#include "ai/openai/codex/frontend/detail/PersistentText.h"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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

    inline constexpr std::string_view ItemContentOverflowV1Property = "aisuiteItemContentOverflowV1";
    inline constexpr std::string_view CommandOutputOverflowV2Property = "aisuiteCommandOutputOverflowV2";
    inline constexpr std::size_t MaximumItemContentOverflowV1Bytes = 32U * 1024U;
    inline constexpr std::size_t MaximumCommandOutputOverflowV2Bytes = 4U * 1024U * 1024U;
    // Base64 chunks make this bound independent of JSON escaping while
    // retaining room in the derived server queue and client frame.
    inline constexpr std::size_t MaximumCommandOutputOverflowV2EncodedBytes = 8U * 1024U * 1024U;

    enum class ItemContentWireMode { Replacement, AppendV1, AppendV2 };

    struct ItemContentOverflowV1 {
        std::uint64_t baseContentBytes = 0;
        std::string suffix;
        std::uint64_t droppedContentBytesBeforeProjection = 0;
        bool contentTruncatedBeforeProjection = false;
        bool truncationBeforeProjection = false;

        bool operator==(const ItemContentOverflowV1&) const = default;
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

    struct TurnPlanStepState {
        std::string step;
        std::string status;

        bool operator==(const TurnPlanStepState&) const = default;
    };

    struct TurnPlanState {
        std::optional<std::string> explanation;
        std::vector<TurnPlanStepState> steps;
        std::size_t totalSteps = 0;
        bool truncated = false;

        bool operator==(const TurnPlanState&) const = default;
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
        std::optional<TurnPlanState> plan;
        SafeDetail safeDetails;
        SafeDetail legacyExtensions;

        TurnState(TurnIdentity turnId, ThreadIdentity parentThreadId)
            : id(std::move(turnId))
            , threadId(std::move(parentThreadId)) {
        }

        bool operator==(const TurnState&) const = default;
    };

    struct UserMessageProjection {
        std::optional<std::string> clientId;
        std::string text;
        bool textTruncated = false;
        bool contentTruncated = false;
        std::uint64_t originalContentBytes = 0;
        std::uint64_t retainedContentBytes = 0;
        std::uint64_t originalContentItems = 0;
        std::uint64_t retainedContentItems = 0;
        std::vector<std::string> textParts;

        bool operator==(const UserMessageProjection&) const = default;
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
        // Connection-neutral suffix retained only so append-v1 recipients can
        // reconstruct agentText beyond the frozen v1 scalar field bound.
        std::optional<ItemContentOverflowV1> agentTextOverflowV1;
        // Append-v2 peers reconstruct the backend-bounded command output from
        // UTF-8 chunks while the frozen v1 commandOutput scalar remains bounded.
        std::optional<ItemContentOverflowV1> commandOutputOverflowV2;
        std::optional<std::uint64_t> droppedContentBytes;
        bool contentTruncated = false;
        std::optional<std::int64_t> startedAtMs;
        std::optional<std::int64_t> completedAtMs;
        std::optional<UserMessageProjection> userMessage;
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

    // Canonical snapshots are immutable once published.  Live content
    // updates therefore need a new snapshot, but copying every retained item
    // (and every growing content string) for each streamed fragment is both
    // unnecessary and prohibitively expensive.  This vector-compatible
    // collection keeps a shared immutable root plus immutable per-index
    // replacements.  Ordinary mutating operations materialize a private
    // vector, while the reducer's content-only path replaces exactly one
    // item without copying the remaining item values.
    class PersistentThreadItems {
    public:
        enum class ContentChannel : std::uint8_t { AgentText, ReasoningText, ReasoningSummary, CommandOutput };

        using value_type = ThreadItem;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using iterator = std::vector<ThreadItem>::iterator;
        using reverse_iterator = std::vector<ThreadItem>::reverse_iterator;

        class const_iterator {
        public:
            using iterator_category = std::random_access_iterator_tag;
            using iterator_concept = std::random_access_iterator_tag;
            using value_type = ThreadItem;
            using difference_type = std::ptrdiff_t;
            using pointer = const ThreadItem*;
            using reference = const ThreadItem&;

            const_iterator() = default;

            [[nodiscard]] reference operator*() const {
                return owner->at(position);
            }
            [[nodiscard]] pointer operator->() const {
                return &owner->at(position);
            }
            reference operator[](difference_type offset) const {
                return owner->at(static_cast<size_type>(static_cast<difference_type>(position) + offset));
            }
            const_iterator& operator++() noexcept {
                ++position;
                return *this;
            }
            const_iterator operator++(int) noexcept {
                const_iterator previous = *this;
                ++*this;
                return previous;
            }
            const_iterator& operator--() noexcept {
                --position;
                return *this;
            }
            const_iterator operator--(int) noexcept {
                const_iterator previous = *this;
                --*this;
                return previous;
            }
            const_iterator& operator+=(difference_type offset) noexcept {
                position = static_cast<size_type>(static_cast<difference_type>(position) + offset);
                return *this;
            }
            const_iterator& operator-=(difference_type offset) noexcept {
                return *this += -offset;
            }
            friend const_iterator operator+(const_iterator value, difference_type offset) noexcept {
                value += offset;
                return value;
            }
            friend const_iterator operator+(difference_type offset, const_iterator value) noexcept {
                value += offset;
                return value;
            }
            friend const_iterator operator-(const_iterator value, difference_type offset) noexcept {
                value -= offset;
                return value;
            }
            friend difference_type operator-(const const_iterator& left, const const_iterator& right) noexcept {
                return static_cast<difference_type>(left.position) - static_cast<difference_type>(right.position);
            }
            friend bool operator==(const const_iterator&, const const_iterator&) = default;
            friend auto operator<=>(const const_iterator& left, const const_iterator& right) noexcept {
                return left.position <=> right.position;
            }

        private:
            friend class PersistentThreadItems;
            const_iterator(const PersistentThreadItems* collection, size_type index) noexcept
                : owner(collection)
                , position(index) {
            }
            const PersistentThreadItems* owner = nullptr;
            size_type position = 0;
        };

        using const_reverse_iterator = std::reverse_iterator<const_iterator>;

        struct ItemLookup {
            std::optional<size_type> firstById;
            std::optional<size_type> scoped;
        };

    private:
        using LookupIndex = std::unordered_map<std::string, std::vector<size_type>>;

        struct LookupState {
            mutable std::mutex mutex;
            mutable std::shared_ptr<LookupIndex> byId;
        };

    public:

        PersistentThreadItems()
            : root(std::make_shared<std::vector<ThreadItem>>())
            , lookupState(std::make_shared<LookupState>()) {
        }
        PersistentThreadItems(std::initializer_list<ThreadItem> values)
            : root(std::make_shared<std::vector<ThreadItem>>(values))
            , lookupState(std::make_shared<LookupState>()) {
        }

        PersistentThreadItems& operator=(std::initializer_list<ThreadItem> values) {
            root = std::make_shared<std::vector<ThreadItem>>(values);
            replacements.clear();
            lookupState = std::make_shared<LookupState>();
            return *this;
        }

        [[nodiscard]] bool empty() const noexcept {
            return root->empty();
        }
        [[nodiscard]] size_type size() const noexcept {
            return root->size();
        }
        [[nodiscard]] const ThreadItem& at(size_type index) const {
            const auto found = replacements.find(index);
            return found == replacements.end() ? (*root)[index] : materialized(*found->second);
        }

        // Content overlays never change item identity, parentage, or kind.
        // Reducer validation can therefore inspect this stable metadata
        // without materializing a growing content string on every append.
        [[nodiscard]] const ThreadItem& metadataAt(size_type index) const {
            const auto found = replacements.find(index);
            return found == replacements.end() ? root->at(index) : *found->second->base;
        }
        [[nodiscard]] ThreadItem& at(size_type index) {
            return mutableValues().at(index);
        }
        [[nodiscard]] const ThreadItem& operator[](size_type index) const {
            return at(index);
        }
        [[nodiscard]] ThreadItem& operator[](size_type index) {
            return mutableValues()[index];
        }
        [[nodiscard]] const ThreadItem& front() const {
            return at(0);
        }
        [[nodiscard]] ThreadItem& front() {
            return mutableValues().front();
        }
        [[nodiscard]] const ThreadItem& back() const {
            return at(size() - 1);
        }
        [[nodiscard]] ThreadItem& back() {
            return mutableValues().back();
        }

        // Content changes preserve item identity, so all snapshots that share
        // this immutable root can also share its lazily built lookup. A
        // structural mutation receives a fresh lookup state in mutableValues().
        [[nodiscard]] ItemLookup lookup(const ItemIdentity& id,
                                        const std::optional<ThreadIdentity>& threadId,
                                        const std::optional<TurnIdentity>& turnId) const {
            std::lock_guard lock(lookupState->mutex);
            if (!lookupState->byId) {
                auto index = std::make_shared<LookupIndex>();
                index->reserve(size());
                for (size_type position = 0; position < size(); ++position) {
                    index->try_emplace(itemData(metadataAt(position)).id.value()).first->second.push_back(position);
                }
                lookupState->byId = std::move(index);
            }
            const auto found = lookupState->byId->find(id.value());
            if (found == lookupState->byId->end() || found->second.empty()) {
                return {};
            }
            ItemLookup result;
            result.firstById = found->second.front();
            for (const size_type position : found->second) {
                const ItemData& item = itemData(metadataAt(position));
                if ((!threadId || item.threadId == threadId) && (!turnId || item.turnId == turnId)) {
                    result.scoped = position;
                    break;
                }
            }
            return result;
        }

        [[nodiscard]] const_iterator begin() const noexcept {
            return const_iterator(this, 0);
        }
        [[nodiscard]] const_iterator end() const noexcept {
            return const_iterator(this, size());
        }
        [[nodiscard]] const_iterator cbegin() const noexcept {
            return begin();
        }
        [[nodiscard]] const_iterator cend() const noexcept {
            return end();
        }
        [[nodiscard]] const_reverse_iterator rbegin() const noexcept {
            return const_reverse_iterator(end());
        }
        [[nodiscard]] const_reverse_iterator rend() const noexcept {
            return const_reverse_iterator(begin());
        }
        [[nodiscard]] iterator begin() {
            return mutableValues().begin();
        }
        [[nodiscard]] iterator end() {
            return mutableValues().end();
        }
        [[nodiscard]] reverse_iterator rbegin() {
            return mutableValues().rbegin();
        }
        [[nodiscard]] reverse_iterator rend() {
            return mutableValues().rend();
        }

        void reserve(size_type capacity) {
            mutableValues().reserve(capacity);
        }
        void clear() {
            mutableValues().clear();
        }
        void push_back(const ThreadItem& item) {
            mutableValues().push_back(item);
        }
        void push_back(ThreadItem&& item) {
            mutableValues().push_back(std::move(item));
        }
        template <typename... Args>
        ThreadItem& emplace_back(Args&&... args) {
            return mutableValues().emplace_back(std::forward<Args>(args)...);
        }
        iterator erase(iterator position) {
            return mutableValues().erase(position);
        }
        iterator erase(iterator first, iterator last) {
            return mutableValues().erase(first, last);
        }
        template <typename Predicate>
        size_type eraseIf(Predicate&& predicate) {
            return std::erase_if(mutableValues(), std::forward<Predicate>(predicate));
        }

        // Preserve the immutable root and all unrelated replacements.
        void replace(size_type index, ThreadItem item) {
            if (index >= size()) {
                throw std::out_of_range("persistent frontend item index");
            }
            auto overlay = std::make_shared<ItemOverlay>();
            overlay->base = std::make_shared<const ThreadItem>(std::move(item));
            replacements[index] = std::move(overlay);
        }

        // Apply an already reducer-validated append without materializing or
        // copying the target's growing string.  The base byte count and UTF-8
        // discard boundary are nevertheless rechecked transactionally here.
        [[nodiscard]] bool appendContent(size_type index,
                                         ContentChannel channel,
                                         std::uint64_t baseContentBytes,
                                         std::uint64_t discardPrefixBytes,
                                         std::string_view delta,
                                         bool contentTruncatedKnown,
                                         bool contentTruncated,
                                         bool droppedContentBytesKnown,
                                         std::optional<std::uint64_t> droppedContentBytes,
                                         std::size_t maximumRetainedBytes) {
            if (index >= size() || baseContentBytes > std::numeric_limits<std::size_t>::max() ||
                discardPrefixBytes > std::numeric_limits<std::size_t>::max()) {
                return false;
            }
            const auto prior = replacements.find(index);
            auto next = std::make_shared<ItemOverlay>();
            if (prior == replacements.end()) {
                next->base = std::shared_ptr<const ThreadItem>(root, &(*root)[index]);
            } else {
                next->base = prior->second->base;
                next->contents = prior->second->contents;
                next->contentTruncatedKnown = prior->second->contentTruncatedKnown;
                next->contentTruncated = prior->second->contentTruncated;
                next->droppedContentBytesKnown = prior->second->droppedContentBytesKnown;
                next->droppedContentBytes = prior->second->droppedContentBytes;
            }

            const std::size_t channelIndex = static_cast<std::size_t>(channel);
            detail::PersistentText current;
            if (next->contents[channelIndex]) {
                current = *next->contents[channelIndex];
            } else {
                const ItemData& data = itemData(*next->base);
                const std::optional<std::string>* initial = content(data, channel);
                current = detail::PersistentText::from(initial && *initial ? std::string_view{**initial} : std::string_view{});
            }
            if (current.size() != static_cast<std::size_t>(baseContentBytes) ||
                discardPrefixBytes > baseContentBytes) {
                return false;
            }
            if (discardPrefixBytes < baseContentBytes) {
                const auto boundary = current.byteAt(static_cast<std::size_t>(discardPrefixBytes));
                if (!boundary || (*boundary & 0xc0U) == 0x80U) {
                    return false;
                }
            }
            const std::size_t retained = current.size() - static_cast<std::size_t>(discardPrefixBytes);
            if (delta.size() > maximumRetainedBytes || retained > maximumRetainedBytes - delta.size()) {
                return false;
            }
            auto appended = current.appended(static_cast<std::size_t>(discardPrefixBytes), delta);
            if (!appended) {
                return false;
            }
            next->contents[channelIndex] = std::move(*appended);
            if (contentTruncatedKnown) {
                next->contentTruncatedKnown = true;
                next->contentTruncated = contentTruncated;
            }
            if (droppedContentBytesKnown) {
                next->droppedContentBytesKnown = true;
                next->droppedContentBytes = droppedContentBytes;
            }
            replacements[index] = std::move(next);
            return true;
        }

        [[nodiscard]] std::vector<ThreadItem> releaseVector() && {
            std::vector<ThreadItem>& values = mutableValues();
            std::vector<ThreadItem> result = std::move(values);
            root = std::make_shared<std::vector<ThreadItem>>();
            return result;
        }

        [[nodiscard]] bool operator==(const PersistentThreadItems& other) const {
            if (size() != other.size()) {
                return false;
            }
            for (size_type index = 0; index < size(); ++index) {
                if (at(index) != other.at(index)) {
                    return false;
                }
            }
            return true;
        }

    private:
        std::vector<ThreadItem>& mutableValues() {
            if (!replacements.empty() || !root.unique()) {
                auto materializedValues = std::make_shared<std::vector<ThreadItem>>(*root);
                for (const auto& [index, replacement] : replacements) {
                    if (index < materializedValues->size()) {
                        (*materializedValues)[index] = materialized(*replacement);
                    }
                }
                root = std::move(materializedValues);
                replacements.clear();
            }
            lookupState = std::make_shared<LookupState>();
            return *root;
        }

        struct ItemOverlay {
            std::shared_ptr<const ThreadItem> base;
            std::array<std::optional<detail::PersistentText>, 4> contents;
            bool contentTruncatedKnown = false;
            bool contentTruncated = false;
            bool droppedContentBytesKnown = false;
            std::optional<std::uint64_t> droppedContentBytes;
            mutable std::mutex mutex;
            mutable std::shared_ptr<const ThreadItem> cached;
        };

        [[nodiscard]] static const std::optional<std::string>* content(const ItemData& data, ContentChannel channel) noexcept {
            switch (channel) {
                case ContentChannel::AgentText:
                    return &data.agentText;
                case ContentChannel::ReasoningText:
                    return &data.reasoningText;
                case ContentChannel::ReasoningSummary:
                    return &data.reasoningSummary;
                case ContentChannel::CommandOutput:
                    return &data.commandOutput;
            }
            return nullptr;
        }

        [[nodiscard]] static std::optional<std::string>* content(ItemData& data, ContentChannel channel) noexcept {
            return const_cast<std::optional<std::string>*>(content(std::as_const(data), channel));
        }

        [[nodiscard]] static const ThreadItem& materialized(const ItemOverlay& overlay) {
            std::lock_guard lock(overlay.mutex);
            if (!overlay.cached) {
                ThreadItem value = *overlay.base;
                ItemData& data = std::visit([](auto& item) -> ItemData& { return item.value; }, value);
                for (std::size_t index = 0; index < overlay.contents.size(); ++index) {
                    if (overlay.contents[index]) {
                        *content(data, static_cast<ContentChannel>(index)) = overlay.contents[index]->materialize();
                    }
                }
                if (overlay.contentTruncatedKnown) {
                    data.contentTruncated = overlay.contentTruncated;
                }
                if (overlay.droppedContentBytesKnown) {
                    data.droppedContentBytes = overlay.droppedContentBytes;
                }
                overlay.cached = std::make_shared<const ThreadItem>(std::move(value));
            }
            return *overlay.cached;
        }

        std::shared_ptr<std::vector<ThreadItem>> root;
        std::unordered_map<size_type, std::shared_ptr<ItemOverlay>> replacements;
        std::shared_ptr<LookupState> lookupState;
    };

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
        PersistentThreadItems items;
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

    [[nodiscard]] ModelResult<Json> encodeItemContentOverflowV1(const ItemContentOverflowV1& overflow) noexcept;
    [[nodiscard]] ModelResult<ItemContentOverflowV1>
    decodeItemContentOverflowV1(const Json& encoded, std::string path) noexcept;
    [[nodiscard]] ModelResult<Json> encodeCommandOutputOverflowV2(const ItemContentOverflowV1& overflow) noexcept;
    [[nodiscard]] ModelResult<ItemContentOverflowV1>
    decodeCommandOutputOverflowV2(const Json& encoded, std::string path) noexcept;
    [[nodiscard]] std::optional<ModelError>
    restoreAgentTextOverflowV1(ItemData& item, const ItemContentOverflowV1& overflow, std::string path) noexcept;
    [[nodiscard]] std::optional<ModelError>
    restoreCommandOutputOverflowV2(ItemData& item, const ItemContentOverflowV1& overflow, std::string path) noexcept;
    [[nodiscard]] std::optional<ModelError>
    restoreItemContentOverflows(ItemData& item,
                                const std::optional<ItemContentOverflowV1>& agentText,
                                const std::optional<ItemContentOverflowV1>& commandOutput,
                                std::string path) noexcept;

    [[nodiscard]] ModelResult<ExpandedSnapshot>
    encodeSnapshot(const CanonicalSnapshot& snapshot, ItemContentWireMode itemContentMode = ItemContentWireMode::Replacement) noexcept;
    [[nodiscard]] ModelResult<CanonicalSnapshot>
    decodeSnapshot(const ExpandedSnapshot& snapshot, ItemContentWireMode itemContentMode = ItemContentWireMode::Replacement) noexcept;
    [[nodiscard]] ModelResult<Snapshot> encodeLegacySnapshot(const CanonicalSnapshot& snapshot) noexcept;
    [[nodiscard]] ModelResult<CanonicalSnapshot>
    decodeLegacySnapshot(const Snapshot& snapshot, ItemContentWireMode itemContentMode = ItemContentWireMode::Replacement) noexcept;

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
                                                                SnapshotRepresentationSelection selection,
                                                                ItemContentWireMode itemContentMode = ItemContentWireMode::Replacement) noexcept;
    [[nodiscard]] ModelResult<Snapshot> encodeProjectedSnapshot(const CanonicalSnapshot& snapshot,
                                                                std::span<const FrontendCapability> selectedCapabilities,
                                                                ItemContentWireMode itemContentMode = ItemContentWireMode::Replacement) noexcept;
    [[nodiscard]] ModelResult<CanonicalSnapshot> decodeProjectedSnapshot(const Snapshot& snapshot,
                                                                         SnapshotRepresentationSelection selection,
                                                                         ItemContentWireMode itemContentMode = ItemContentWireMode::Replacement) noexcept;
    [[nodiscard]] ModelResult<CanonicalSnapshot> decodeProjectedSnapshot(const Snapshot& snapshot,
                                                                         std::span<const FrontendCapability> selectedCapabilities,
                                                                         ItemContentWireMode itemContentMode = ItemContentWireMode::Replacement) noexcept;

    static_assert(std::variant_size_v<ThreadItem> == 18);
    static_assert(std::variant_size_v<PendingRequest> == 10);

} // namespace ai::openai::codex::frontend::internal::model

#endif // AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_MODEL_H
