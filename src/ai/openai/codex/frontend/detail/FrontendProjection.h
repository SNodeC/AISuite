/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_DETAIL_FRONTENDPROJECTION_H
#define AI_OPENAI_CODEX_FRONTEND_DETAIL_FRONTENDPROJECTION_H

#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/Messages.h"
#include "ai/openai/codex/frontend/Security.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ai::openai::codex::frontend::detail {

    // These limits are a final defensive boundary. BackendCore snapshots and
    // events are already bounded, but the canonical frontend journal must not
    // rely on that implementation detail when accepting compatibility data.
    struct FrontendProjectionLimits {
        std::size_t maximumDepth = 64;
        std::size_t maximumVisits = 262'144;
        std::size_t maximumObjectMembers = 4'096;
        std::size_t maximumArrayItems = 4'096;
        std::size_t maximumPropertyNameBytes = 256;
        std::size_t maximumStringBytes = 64U * 1'024U;
        std::size_t maximumProjectionRules = 128;
        std::size_t maximumReportedPaths = 64;
    };

    struct CanonicalSanitizationStatistics {
        std::size_t visits = 0;
        std::size_t maximumDepthObserved = 0;
        std::size_t knownStructuredSecretFieldsRemoved = 0;
        std::size_t unsafeRawFieldsRemoved = 0;
        std::size_t valuesOmittedByBounds = 0;
        std::size_t stringsTruncated = 0;
        bool truncated = false;
        bool failed = false;
    };

    enum class ScopeProjectionAction { Omit, Redact };

    // Paths use JSON-pointer spelling. A complete path component may be '*'
    // to match one object member or array element. All listed scopes are
    // required. Rules are applied after unconditional canonical scrubbing.
    struct ScopeProjectionRule {
        std::string path;
        std::vector<FrontendScope> requiredScopes;
        ScopeProjectionAction action = ScopeProjectionAction::Omit;

        bool operator==(const ScopeProjectionRule&) const = default;
    };

    struct ScopedProjectionValue {
        Json value = Json::object();
        std::vector<ScopeProjectionRule> rules;

        bool operator==(const ScopedProjectionValue&) const = default;
    };

    struct FrontendProjectionContext {
        std::vector<FrontendScope> scopes;
        std::vector<FrontendCapability> capabilities;

        [[nodiscard]] bool hasScope(FrontendScope scope) const noexcept;
        [[nodiscard]] bool hasCapability(FrontendCapability capability) const noexcept;
        [[nodiscard]] bool hasAllScopes(std::span<const FrontendScope> required) const noexcept;
    };

    [[nodiscard]] FrontendProjectionContext makeProjectionContext(const FrontendPrincipal& principal,
                                                                  std::span<const FrontendCapability> negotiatedCapabilities = {}) noexcept;

    struct CanonicalSnapshotRecord {
        SequenceNumber sequence;
        ScopedProjectionValue legacyState;
        ScopedProjectionValue expandedState;
        Json extensions = Json::object();
        CanonicalSanitizationStatistics sanitization;
        std::size_t maximumReportedPaths = 64;
    };

    struct CanonicalExpandedEvent {
        ExpandedEventType type = ExpandedEventType::DiagnosticsUpdated;
        ScopedProjectionValue data;

        bool operator==(const CanonicalExpandedEvent&) const = default;
    };

    // One provider/backend occurrence is retained once and owns one global
    // sequence. Every dedicated family and the legacy compatibility view are
    // projections of that same record and therefore carry the same sequence.
    struct CanonicalEventRecord {
        SequenceNumber sequence;
        std::string legacyType;
        ScopedProjectionValue legacyData;
        std::vector<CanonicalExpandedEvent> expandedEvents;
        FrontendCapability expansionCapability = FrontendCapability::DedicatedNotificationEvents;
        std::vector<FrontendScope> requiredScopes{FrontendScope::Observe};
        std::optional<std::string> registryKey;
        Json extensions = Json::object();
        CanonicalSanitizationStatistics sanitization;
        std::size_t maximumReportedPaths = 64;
    };

    struct SnapshotProjection {
        Snapshot snapshot;
        bool expanded = false;
        std::vector<std::string> omittedFields;
        std::vector<std::string> redactedFields;
    };

    struct EventProjection {
        std::vector<FrontendEvent> events;
        bool expanded = false;
        std::vector<std::string> omittedFields;
        std::vector<std::string> redactedFields;
    };

    [[nodiscard]] CanonicalSnapshotRecord canonicalizeSnapshot(CanonicalSnapshotRecord input,
                                                               FrontendProjectionLimits limits = {}) noexcept;
    [[nodiscard]] CanonicalEventRecord canonicalizeEvent(CanonicalEventRecord input, FrontendProjectionLimits limits = {}) noexcept;

    [[nodiscard]] std::optional<SnapshotProjection> projectSnapshot(const CanonicalSnapshotRecord& record,
                                                                    const FrontendProjectionContext& context) noexcept;
    [[nodiscard]] EventProjection projectEvent(const CanonicalEventRecord& record,
                                               const FrontendProjectionContext& context,
                                               std::optional<SequenceNumber> replayAfter = std::nullopt) noexcept;

    [[nodiscard]] std::optional<std::size_t> canonicalEventRetainedBytes(const CanonicalEventRecord& record) noexcept;

    [[nodiscard]] bool canonicalValueContainsNoKnownStructuredSecrets(const Json& value) noexcept;

    struct ExpandedEventProjectionMetadata {
        ExpandedEventType type;
        std::optional<FrontendScope> privilegedScope;
    };

    inline constexpr std::array<ExpandedEventProjectionMetadata, 26> AllExpandedEventProjections{{
        {ExpandedEventType::ProviderUpdated, std::nullopt},
        {ExpandedEventType::ControllerUpdated, std::nullopt},
        {ExpandedEventType::SessionsUpdated, std::nullopt},
        {ExpandedEventType::ThreadListUpdated, std::nullopt},
        {ExpandedEventType::ThreadUpserted, std::nullopt},
        {ExpandedEventType::ThreadRemoved, std::nullopt},
        {ExpandedEventType::TurnUpserted, std::nullopt},
        {ExpandedEventType::ItemUpserted, std::nullopt},
        {ExpandedEventType::ItemContentUpdated, std::nullopt},
        // Pending-request presentation is an Observe contract. Responding to
        // one remains independently gated by Control + SensitiveResponse and
        // controller ownership in generated method policy.
        {ExpandedEventType::PendingRequestsUpdated, std::nullopt},
        {ExpandedEventType::AccountUpdated, std::nullopt},
        {ExpandedEventType::ModelsUpdated, std::nullopt},
        {ExpandedEventType::ConfigurationUpdated, std::nullopt},
        {ExpandedEventType::ProcessUpdated, FrontendScope::CommandExecution},
        {ExpandedEventType::FilesystemWatchUpdated, FrontendScope::FilesystemRead},
        {ExpandedEventType::FuzzySearchUpdated, FrontendScope::FilesystemRead},
        {ExpandedEventType::ReviewsUpdated, std::nullopt},
        {ExpandedEventType::IntegrationsUpdated, std::nullopt},
        {ExpandedEventType::PluginsUpdated, std::nullopt},
        {ExpandedEventType::SkillsUpdated, std::nullopt},
        {ExpandedEventType::McpUpdated, std::nullopt},
        {ExpandedEventType::PlatformUpdated, std::nullopt},
        {ExpandedEventType::NoticeAdded, std::nullopt},
        {ExpandedEventType::ActivityUpdated, std::nullopt},
        {ExpandedEventType::CapacityUpdated, std::nullopt},
        {ExpandedEventType::DiagnosticsUpdated, std::nullopt},
    }};

    [[nodiscard]] const generated::ProjectionMetadata* notificationProjection(std::string_view registryKey) noexcept;
    [[nodiscard]] const generated::ProjectionMetadata* threadItemProjection(std::string_view registryKey) noexcept;
    [[nodiscard]] const generated::PendingRequestProjectionMetadata* pendingRequestProjection(PendingRequestKind kind) noexcept;
    [[nodiscard]] bool projectionMetadataIsComplete() noexcept;

    static_assert(generated::AllPendingRequestProjections.size() == 10);
    static_assert(AllExpandedEventProjections.size() == 26);

} // namespace ai::openai::codex::frontend::detail

#endif // AI_OPENAI_CODEX_FRONTEND_DETAIL_FRONTENDPROJECTION_H
