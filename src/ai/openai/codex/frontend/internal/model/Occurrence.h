/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_OCCURRENCE_H
#define AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_OCCURRENCE_H

#include "ai/openai/codex/frontend/internal/model/Model.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>

namespace ai::openai::codex::frontend::internal::model {

    struct OccurrenceIdentity {
        FrontendSequence sequence;
        OccurrenceGroupIdentity groupId;
        std::uint32_t groupIndex = 0;
        std::uint32_t groupCount = 1;
        SourceStamp sourceStamp;
        std::optional<ProjectionStamp> projectionStamp;
        std::optional<SessionIdentity> sessionId;
        std::optional<ControllerIdentity> controllerId;
        std::optional<ThreadIdentity> threadId;
        std::optional<TurnIdentity> turnId;
        std::optional<ItemIdentity> itemId;
        std::optional<PendingRequestIdentity> pendingRequestId;
        std::optional<ProcessHandle> processHandle;

        OccurrenceIdentity(FrontendSequence occurrenceSequence,
                           OccurrenceGroupIdentity occurrenceGroupId,
                           std::uint32_t occurrenceGroupIndex,
                           std::uint32_t occurrenceGroupCount,
                           SourceStamp occurrenceSourceStamp)
            : sequence(occurrenceSequence)
            , groupId(std::move(occurrenceGroupId))
            , groupIndex(occurrenceGroupIndex)
            , groupCount(occurrenceGroupCount)
            , sourceStamp(std::move(occurrenceSourceStamp)) {
        }

        [[nodiscard]] bool valid() const noexcept;

        bool operator==(const OccurrenceIdentity&) const = default;
    };

    struct ProviderUpdatedOccurrence {
        ProviderState provider;
        SafeDetail extensions;
        explicit ProviderUpdatedOccurrence(ProviderState value)
            : provider(std::move(value)) {
        }
        bool operator==(const ProviderUpdatedOccurrence&) const = default;
    };

    struct ControllerUpdatedOccurrence {
        ControllerState controller;
        SafeDetail extensions;
        explicit ControllerUpdatedOccurrence(ControllerState value)
            : controller(std::move(value)) {
        }
        bool operator==(const ControllerUpdatedOccurrence&) const = default;
    };

    struct SessionsUpdatedOccurrence {
        std::vector<SessionState> sessions;
        std::optional<SessionState> changedSession;
        std::optional<bool> connected;
        bool completeProjection = true;
        SafeDetail extensions;
        explicit SessionsUpdatedOccurrence(std::vector<SessionState> value)
            : sessions(std::move(value)) {
        }
        SessionsUpdatedOccurrence() = default;
        bool operator==(const SessionsUpdatedOccurrence&) const = default;
    };

    struct ThreadListUpdatedOccurrence {
        ThreadListState threadList;
        SafeDetail extensions;
        explicit ThreadListUpdatedOccurrence(ThreadListState value)
            : threadList(std::move(value)) {
        }
        bool operator==(const ThreadListUpdatedOccurrence&) const = default;
    };

    struct ThreadUpsertedOccurrence {
        ThreadState thread;
        std::vector<TurnState> turns;
        std::vector<ThreadItem> items;
        bool replaceDescendants = false;
        SafeDetail extensions;
        explicit ThreadUpsertedOccurrence(ThreadState value)
            : thread(std::move(value)) {
        }
        bool operator==(const ThreadUpsertedOccurrence&) const = default;
    };

    struct ThreadRemovedOccurrence {
        ThreadIdentity threadId;
        SafeDetail extensions;
        explicit ThreadRemovedOccurrence(ThreadIdentity value)
            : threadId(std::move(value)) {
        }
        bool operator==(const ThreadRemovedOccurrence&) const = default;
    };

    struct TurnUpsertedOccurrence {
        TurnState turn;
        std::vector<ThreadItem> items;
        bool replaceItems = false;
        SafeDetail extensions;
        explicit TurnUpsertedOccurrence(TurnState value)
            : turn(std::move(value)) {
        }
        bool operator==(const TurnUpsertedOccurrence&) const = default;
    };

    struct ItemUpsertedOccurrence {
        ThreadItem item;
        SafeDetail extensions;
        explicit ItemUpsertedOccurrence(ThreadItem value)
            : item(std::move(value)) {
        }
        bool operator==(const ItemUpsertedOccurrence&) const = default;
    };

    struct ItemContentAppendHint {
        std::uint64_t baseContentBytes = 0;
        std::string delta;
        // Set only when projection verified this hint against the bounded
        // authoritative backend channel rather than the frozen v1 prefix.
        bool sourceVerified = false;

        bool operator==(const ItemContentAppendHint&) const = default;
    };

    struct ItemContentUpdatedOccurrence {
        std::optional<ThreadIdentity> threadId;
        std::optional<TurnIdentity> turnId;
        ItemIdentity itemId;
        std::optional<std::string> channel;
        std::optional<std::string> content;
        TruncationMetadata truncation;
        bool contentTruncatedKnown = true;
        bool droppedContentBytesKnown = true;
        std::optional<ItemContentOverflowV1> overflowV1;
        // The full replacement is authoritative in server/journal values.
        // This hint is connection-neutral until an encoder is explicitly
        // asked to use append-v1 for one negotiated recipient.
        std::optional<ItemContentAppendHint> appendHint;
        // True only for an append-v1 occurrence decoded from the wire.
        bool appendWireRepresentation = false;
        // True only for a validated append-v1 overflow replacement decoded
        // from the schema-neutral reserved detail member.
        bool overflowWireRepresentation = false;
        SafeDetail extensions;
        explicit ItemContentUpdatedOccurrence(ItemIdentity value)
            : itemId(std::move(value)) {
        }
        bool operator==(const ItemContentUpdatedOccurrence&) const = default;
    };

    struct PendingRequestsUpdatedOccurrence {
        std::vector<PendingRequest> pendingRequests;
        std::optional<PendingRequestIdentity> removedRequestId;
        std::optional<std::string> resolutionReason;
        bool completeProjection = true;
        SafeDetail extensions;
        explicit PendingRequestsUpdatedOccurrence(std::vector<PendingRequest> value)
            : pendingRequests(std::move(value)) {
        }
        PendingRequestsUpdatedOccurrence() = default;
        bool operator==(const PendingRequestsUpdatedOccurrence&) const = default;
    };

#define AISUITE_CODEX_FRONTEND_DOMAIN_OCCURRENCE(name, domainType, memberName)                                                             \
    struct name {                                                                                                                          \
        domainType memberName;                                                                                                             \
        SafeDetail extensions;                                                                                                             \
        explicit name(domainType value)                                                                                                    \
            : memberName(std::move(value)) {                                                                                               \
        }                                                                                                                                  \
        bool operator==(const name&) const = default;                                                                                      \
    }

    AISUITE_CODEX_FRONTEND_DOMAIN_OCCURRENCE(AccountUpdatedOccurrence, AccountsState, account);
    AISUITE_CODEX_FRONTEND_DOMAIN_OCCURRENCE(ModelsUpdatedOccurrence, ModelsState, models);
    AISUITE_CODEX_FRONTEND_DOMAIN_OCCURRENCE(ConfigurationUpdatedOccurrence, ConfigurationState, configuration);

    struct ProcessUpdatedOccurrence {
        ProcessState process;
        SafeDetail extensions;
        explicit ProcessUpdatedOccurrence(ProcessState value)
            : process(std::move(value)) {
        }
        bool operator==(const ProcessUpdatedOccurrence&) const = default;
    };

    struct FilesystemWatchUpdatedOccurrence {
        FilesystemWatchRecord filesystemWatch;
        SafeDetail extensions;
        explicit FilesystemWatchUpdatedOccurrence(FilesystemWatchRecord value)
            : filesystemWatch(std::move(value)) {
        }
        bool operator==(const FilesystemWatchUpdatedOccurrence&) const = default;
    };

    struct FuzzySearchUpdatedOccurrence {
        FuzzySearchRecord fuzzySearch;
        SafeDetail extensions;
        explicit FuzzySearchUpdatedOccurrence(FuzzySearchRecord value)
            : fuzzySearch(std::move(value)) {
        }
        bool operator==(const FuzzySearchUpdatedOccurrence&) const = default;
    };

    AISUITE_CODEX_FRONTEND_DOMAIN_OCCURRENCE(ReviewsUpdatedOccurrence, ReviewsState, reviews);
    AISUITE_CODEX_FRONTEND_DOMAIN_OCCURRENCE(IntegrationsUpdatedOccurrence, IntegrationsState, integrations);
    AISUITE_CODEX_FRONTEND_DOMAIN_OCCURRENCE(PluginsUpdatedOccurrence, PluginsState, plugins);
    AISUITE_CODEX_FRONTEND_DOMAIN_OCCURRENCE(SkillsUpdatedOccurrence, SkillsState, skills);
    AISUITE_CODEX_FRONTEND_DOMAIN_OCCURRENCE(McpUpdatedOccurrence, McpState, mcp);
    AISUITE_CODEX_FRONTEND_DOMAIN_OCCURRENCE(PlatformUpdatedOccurrence, PlatformState, platform);

#undef AISUITE_CODEX_FRONTEND_DOMAIN_OCCURRENCE

    struct NoticeAddedOccurrence {
        NoticeRecord notice;
        SafeDetail extensions;
        explicit NoticeAddedOccurrence(NoticeRecord value)
            : notice(std::move(value)) {
        }
        bool operator==(const NoticeAddedOccurrence&) const = default;
    };

    struct ActivityUpdatedOccurrence {
        ActivityRecord activity;
        SafeDetail extensions;
        explicit ActivityUpdatedOccurrence(ActivityRecord value)
            : activity(std::move(value)) {
        }
        bool operator==(const ActivityUpdatedOccurrence&) const = default;
    };

    struct CapacityUpdatedOccurrence {
        CapacityState capacity;
        SafeDetail extensions;
        explicit CapacityUpdatedOccurrence(CapacityState value)
            : capacity(std::move(value)) {
        }
        bool operator==(const CapacityUpdatedOccurrence&) const = default;
    };

    struct DiagnosticsUpdatedOccurrence {
        DiagnosticRecord diagnostic;
        std::vector<DiagnosticRecord> aggregateEntries;
        bool aggregateLegacyUpdate = false;
        SafeDetail extensions;
        explicit DiagnosticsUpdatedOccurrence(DiagnosticRecord value)
            : diagnostic(std::move(value)) {
        }
        bool operator==(const DiagnosticsUpdatedOccurrence&) const = default;
    };

    using OccurrencePayload = std::variant<ProviderUpdatedOccurrence,
                                           ControllerUpdatedOccurrence,
                                           SessionsUpdatedOccurrence,
                                           ThreadListUpdatedOccurrence,
                                           ThreadUpsertedOccurrence,
                                           ThreadRemovedOccurrence,
                                           TurnUpsertedOccurrence,
                                           ItemUpsertedOccurrence,
                                           ItemContentUpdatedOccurrence,
                                           PendingRequestsUpdatedOccurrence,
                                           AccountUpdatedOccurrence,
                                           ModelsUpdatedOccurrence,
                                           ConfigurationUpdatedOccurrence,
                                           ProcessUpdatedOccurrence,
                                           FilesystemWatchUpdatedOccurrence,
                                           FuzzySearchUpdatedOccurrence,
                                           ReviewsUpdatedOccurrence,
                                           IntegrationsUpdatedOccurrence,
                                           PluginsUpdatedOccurrence,
                                           SkillsUpdatedOccurrence,
                                           McpUpdatedOccurrence,
                                           PlatformUpdatedOccurrence,
                                           NoticeAddedOccurrence,
                                           ActivityUpdatedOccurrence,
                                           CapacityUpdatedOccurrence,
                                           DiagnosticsUpdatedOccurrence>;

    enum class LegacyCompatibilityKind {
        ProviderChanged,
        ControllerChanged,
        SessionChanged,
        ThreadListUpdated,
        ThreadUpdated,
        ThreadRemoved,
        TurnUpdated,
        ItemUpdated,
        ItemContentUpdated,
        PendingRequestAdded,
        PendingRequestResolved,
        DiagnosticsUpdated,
        DirectExpanded,
        CodexExtension,
        LegacyItem,
        LegacyPendingRequest
    };

    struct LegacySafeExtension {
        struct FieldTruncation {
            std::optional<std::uint64_t> originalBytes;
            std::optional<std::uint64_t> retainedBytes;
            SafeDetail extensions;

            [[nodiscard]] bool empty() const noexcept {
                return !originalBytes.has_value() && !retainedBytes.has_value() && extensions.empty();
            }

            bool operator==(const FieldTruncation&) const = default;
        };

        struct WireTruncation {
            std::optional<FieldTruncation> method;
            std::optional<FieldTruncation> params;
            std::optional<FieldTruncation> decodingError;
            SafeDetail extensions;

            [[nodiscard]] bool empty() const noexcept {
                return !method.has_value() && !params.has_value() && !decodingError.has_value() && extensions.empty();
            }

            bool operator==(const WireTruncation&) const = default;
        };

        std::string method;
        SafeDetail params;
        std::optional<std::string> decodingError;
        bool sensitiveFieldsRedacted = false;
        TruncationMetadata truncation;
        SafeDetail extensions;
        WireTruncation wireTruncation;
        bool paramsKnown = true;

        bool operator==(const LegacySafeExtension&) const = default;
    };

    [[nodiscard]] Json encodeLegacyExtensionTruncation(const LegacySafeExtension& extension);

    struct LegacyCompatibilityPayload {
        LegacyCompatibilityKind kind = LegacyCompatibilityKind::DirectExpanded;
        std::size_t sourcePayloadIndex = 0;
        std::optional<SessionIdentity> changedSessionId;
        bool connected = true;
        std::optional<PendingRequestIdentity> resolvedRequestId;
        std::optional<std::string> resolutionReason;
        std::optional<LegacySafeExtension> safeExtension;
        std::optional<LegacyItemCompatibility> legacyItem;
        std::optional<LegacyPendingRequestCompatibility> legacyPendingRequest;
        SafeDetail extensions;

        bool operator==(const LegacyCompatibilityPayload&) const = default;
    };

    [[nodiscard]] ExpandedEventType occurrenceType(const OccurrencePayload& payload) noexcept;
    [[nodiscard]] const SafeDetail& occurrenceExtensions(const OccurrencePayload& payload) noexcept;

    enum class OccurrenceErrorCode {
        InvalidGroup,
        InvalidPayload,
        UnsafeDetail,
        UnsupportedFamily,
        AmbiguousLegacyFamily,
        EncodingFailure
    };

    struct OccurrenceError {
        OccurrenceErrorCode code = OccurrenceErrorCode::InvalidPayload;
        std::string path;
        std::string message;

        bool operator==(const OccurrenceError&) const = default;
    };

    template <typename Value>
    class OccurrenceResult {
    public:
        OccurrenceResult(Value value)
            : result(std::move(value)) {
        }

        OccurrenceResult(OccurrenceError error)
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

        [[nodiscard]] const OccurrenceError& error() const& {
            return std::get<OccurrenceError>(result);
        }

    private:
        std::variant<Value, OccurrenceError> result;
    };

    class CanonicalOccurrence {
    public:
        CanonicalOccurrence(const CanonicalOccurrence&) = default;
        CanonicalOccurrence(CanonicalOccurrence&&) noexcept = default;
        CanonicalOccurrence& operator=(const CanonicalOccurrence&) = default;
        CanonicalOccurrence& operator=(CanonicalOccurrence&&) noexcept = default;

        [[nodiscard]] const OccurrenceIdentity& identity() const noexcept;
        [[nodiscard]] const LegacyCompatibilityPayload& legacyCompatibility() const noexcept;
        [[nodiscard]] const std::vector<OccurrencePayload>& expandedPayloads() const noexcept;

        bool operator==(const CanonicalOccurrence&) const = default;

    private:
        friend OccurrenceResult<CanonicalOccurrence> makeOccurrence(OccurrenceIdentity identity, OccurrencePayload payload) noexcept;
        friend OccurrenceResult<CanonicalOccurrence> makeOccurrenceGroup(OccurrenceIdentity identity,
                                                                         LegacyCompatibilityPayload legacy,
                                                                         std::vector<OccurrencePayload> expanded) noexcept;

        CanonicalOccurrence(OccurrenceIdentity identity, LegacyCompatibilityPayload legacy, std::vector<OccurrencePayload> expanded);

        OccurrenceIdentity occurrenceIdentity;
        LegacyCompatibilityPayload legacyPayload;
        std::vector<OccurrencePayload> occurrencePayloads;
    };

    using CanonicalOccurrenceGroup = CanonicalOccurrence;

    [[nodiscard]] OccurrenceResult<CanonicalOccurrence> makeOccurrence(OccurrenceIdentity identity, OccurrencePayload payload) noexcept;
    [[nodiscard]] OccurrenceResult<CanonicalOccurrence>
    makeOccurrenceGroup(OccurrenceIdentity identity, LegacyCompatibilityPayload legacy, std::vector<OccurrencePayload> expanded) noexcept;
    [[nodiscard]] bool validateOccurrenceGroup(std::span<const OccurrencePayload> expanded, OccurrenceError* error = nullptr) noexcept;
    [[nodiscard]] bool validateOccurrenceGroup(std::span<const CanonicalOccurrence> members, OccurrenceError* error = nullptr) noexcept;
    [[nodiscard]] OccurrenceResult<CanonicalOccurrence> mergeOccurrenceGroup(std::span<const CanonicalOccurrence> members) noexcept;

    struct OccurrenceDraft {
        SourceStamp sourceStamp;
        LegacyCompatibilityPayload legacyCompatibility;
        std::vector<OccurrencePayload> expandedPayloads;
        std::optional<ProjectionStamp> projectionStamp;
        std::optional<SessionIdentity> sessionId;
        std::optional<ControllerIdentity> controllerId;
        std::optional<ThreadIdentity> threadId;
        std::optional<TurnIdentity> turnId;
        std::optional<ItemIdentity> itemId;
        std::optional<PendingRequestIdentity> pendingRequestId;
        std::optional<ProcessHandle> processHandle;

        OccurrenceDraft(SourceStamp occurrenceSourceStamp, OccurrencePayload occurrencePayload);

        OccurrenceDraft(SourceStamp occurrenceSourceStamp, LegacyCompatibilityPayload legacy, std::vector<OccurrencePayload> expanded)
            : sourceStamp(std::move(occurrenceSourceStamp))
            , legacyCompatibility(std::move(legacy))
            , expandedPayloads(std::move(expanded)) {
        }

        bool operator==(const OccurrenceDraft&) const = default;
    };

    // Empty expanded payloads are valid only for the bounded, future-safe
    // codex.extension fallback.  Known occurrences remain closed over the 26
    // generated expanded families.
    [[nodiscard]] bool validateOccurrenceDraft(const OccurrenceDraft& draft, OccurrenceError* error = nullptr) noexcept;

    struct OccurrenceDecodeContext {
        OccurrenceGroupIdentity groupId;
        std::uint32_t groupIndex = 0;
        std::uint32_t groupCount = 1;
        SourceStamp sourceStamp;
        std::optional<ProjectionStamp> projectionStamp;
        std::optional<SessionIdentity> sessionId;
        std::optional<ControllerIdentity> controllerId;
        std::optional<ThreadIdentity> threadId;
        std::optional<TurnIdentity> turnId;
        std::optional<ItemIdentity> itemId;
        std::optional<PendingRequestIdentity> pendingRequestId;
        std::optional<ProcessHandle> processHandle;

        OccurrenceDecodeContext(OccurrenceGroupIdentity occurrenceGroupId,
                                std::uint32_t occurrenceGroupIndex,
                                std::uint32_t occurrenceGroupCount,
                                SourceStamp occurrenceSourceStamp)
            : groupId(std::move(occurrenceGroupId))
            , groupIndex(occurrenceGroupIndex)
            , groupCount(occurrenceGroupCount)
            , sourceStamp(std::move(occurrenceSourceStamp)) {
        }
    };

    [[nodiscard]] OccurrenceResult<std::vector<ExpandedFrontendEvent>>
    encodeExpandedOccurrence(const CanonicalOccurrence& occurrence,
                             ItemContentWireMode itemContentMode = ItemContentWireMode::Replacement) noexcept;
    [[nodiscard]] OccurrenceResult<FrontendEvent> encodeLegacyOccurrence(const CanonicalOccurrence& occurrence) noexcept;
    [[nodiscard]] OccurrenceResult<CanonicalOccurrence> decodeExpandedOccurrence(const ExpandedFrontendEvent& event,
                                                                                 const OccurrenceDecodeContext& context,
                                                                                 ItemContentWireMode itemContentMode =
                                                                                     ItemContentWireMode::Replacement) noexcept;
    [[nodiscard]] OccurrenceResult<CanonicalOccurrence>
    decodeLegacyOccurrence(const FrontendEvent& event,
                           const OccurrenceDecodeContext& context,
                           std::optional<ExpandedEventType> familyHint = std::nullopt) noexcept;

    [[nodiscard]] ModelResult<CanonicalSnapshot> reduceOccurrence(const CanonicalSnapshot& snapshot,
                                                                  const CanonicalOccurrence& occurrence) noexcept;
    // Mutates a caller-owned transactional candidate. Callers must discard
    // that candidate if an error is returned; reduceOccurrence() remains the
    // copy-preserving convenience boundary for standalone reductions.
    [[nodiscard]] ModelResult<bool> applyOccurrence(CanonicalSnapshot& candidate,
                                                    const CanonicalOccurrence& occurrence) noexcept;

    static_assert(std::variant_size_v<OccurrencePayload> == 26);

} // namespace ai::openai::codex::frontend::internal::model

#endif // AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_OCCURRENCE_H
