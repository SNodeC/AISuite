/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_INTERNAL_SERVER_BACKENDPROJECTION_H
#define AI_OPENAI_CODEX_FRONTEND_INTERNAL_SERVER_BACKENDPROJECTION_H

#include "ai/openai/codex/backend/BackendEvent.h"
#include "ai/openai/codex/backend/Snapshot.h"
#include "ai/openai/codex/frontend/internal/server/ServerCore.h"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ai::openai::codex::frontend::internal::server {

    struct BackendProjectionTestAccess;

    struct ProjectedBackendOccurrence {
        OccurrenceCoalescingKey key;
        model::OccurrenceDraft occurrence;
        OccurrenceFlushUrgency urgency = OccurrenceFlushUrgency::Deferred;

        bool operator==(const ProjectedBackendOccurrence&) const = default;
    };

    struct ProjectedBackendBatch {
        model::CanonicalSnapshot snapshot;
        std::vector<ProjectedBackendOccurrence> occurrences;
        bool snapshotRequired = false;

        bool operator==(const ProjectedBackendBatch&) const = default;
    };

    // Greenfield backend boundary for the server core. A backend snapshot is
    // converted once into the canonical typed model. Event normalization then
    // selects payloads from that same converted state, so snapshot, live, and
    // replay cannot acquire parallel semantic truths.
    class BackendProjection {
    public:
        [[nodiscard]] model::ModelResult<model::CanonicalSnapshot>
        projectSnapshot(const backend::Snapshot& snapshot) const noexcept;

        [[nodiscard]] model::ModelResult<ProjectedBackendBatch>
        projectOccurrences(std::span<const backend::SequencedBackendEvent> events,
                           const backend::Snapshot& snapshot) const noexcept;

        // Streaming content changes can be projected from the affected item
        // snapshots alone. Callers fall back atomically to projectOccurrences
        // when this exact-entity fast path cannot represent the whole batch.
        [[nodiscard]] model::ModelResult<ProjectedBackendBatch>
        projectItemContentOccurrences(std::span<const backend::SequencedBackendEvent> events,
                                      std::span<const backend::ItemContentSnapshot> items) const noexcept;

    private:
        friend struct BackendProjectionTestAccess;

        // Activity records currently have no generated backend-notification
        // mapping. Keep this narrowly scoped seam private so the exact-entity
        // containment invariant remains executable without creating a second
        // protocol or projection registry.
        [[nodiscard]] model::ModelResult<ProjectedBackendBatch>
        projectActivityForTesting(const backend::Snapshot& snapshot, std::optional<std::string_view> key) const noexcept;
    };

} // namespace ai::openai::codex::frontend::internal::server

#endif // AI_OPENAI_CODEX_FRONTEND_INTERNAL_SERVER_BACKENDPROJECTION_H
