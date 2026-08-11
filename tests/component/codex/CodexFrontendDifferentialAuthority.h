/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef TESTS_COMPONENT_CODEX_CODEXFRONTENDDIFFERENTIALAUTHORITY_H
#define TESTS_COMPONENT_CODEX_CODEXFRONTENDDIFFERENTIALAUTHORITY_H

#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/internal/model/Occurrence.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace tests::codex {

    namespace frontend = ai::openai::codex::frontend;
    namespace generated = ai::openai::codex::frontend::generated;
    namespace model = ai::openai::codex::frontend::internal::model;

    inline model::OccurrenceResult<model::CanonicalOccurrence>
    withAuthorityLegacyCompatibility(const model::CanonicalOccurrence& occurrence) noexcept {
        if (occurrence.legacyCompatibility().kind != model::LegacyCompatibilityKind::DirectExpanded ||
            occurrence.expandedPayloads().empty()) {
            return occurrence;
        }

        const std::string family(frontend::toString(model::occurrenceType(occurrence.expandedPayloads().front())));
        const auto authority = std::find_if(
            generated::AllNotificationProjections.begin(),
            generated::AllNotificationProjections.end(),
            [&](const generated::ProjectionMetadata& metadata) {
                // A family-only fixture cannot truthfully claim a generated
                // multi-family source occurrence.  Use generated authority
                // only when that source maps to this one family; otherwise
                // retain an explicitly synthetic, bounded compatibility
                // method for the representation-level reducer case.
                return metadata.legacyContract == "legacy_redacted_extension" && metadata.expandedMappings.size() == 1 &&
                       std::find(metadata.expandedMappings.begin(), metadata.expandedMappings.end(), family) !=
                           metadata.expandedMappings.end();
            });

        constexpr std::string_view MethodPrefix = "server_notification:ServerNotification:method:";
        std::string method = "frontend/" + family;
        model::OccurrenceIdentity identity = occurrence.identity();
        if (authority != generated::AllNotificationProjections.end() && authority->registryKey.starts_with(MethodPrefix)) {
            method = std::string(authority->registryKey.substr(MethodPrefix.size()));
            identity.sourceStamp = model::SourceStamp{std::string(authority->registryKey)};
        } else {
            identity.sourceStamp = model::SourceStamp{"differential_compatibility:" + family};
        }

        model::LegacyCompatibilityPayload legacy;
        legacy.kind = model::LegacyCompatibilityKind::CodexExtension;
        legacy.safeExtension = model::LegacySafeExtension{};
        legacy.safeExtension->method = std::move(method);
        return model::makeOccurrenceGroup(identity, std::move(legacy), occurrence.expandedPayloads());
    }

} // namespace tests::codex

#endif // TESTS_COMPONENT_CODEX_CODEXFRONTENDDIFFERENTIALAUTHORITY_H
