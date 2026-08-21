/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_SNAPSHOTPIPELINEINSTRUMENTATION_H
#define AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_SNAPSHOTPIPELINEINSTRUMENTATION_H

#include <cstddef>

namespace ai::openai::codex::frontend::internal::model::detail {

    struct SnapshotPipelineInstrumentation {
        std::size_t legacyStateBuilds = 0;
        std::size_t expandedStateBuilds = 0;
        std::size_t filteredProjections = 0;
        std::size_t passThroughProjections = 0;

        bool operator==(const SnapshotPipelineInstrumentation&) const = default;
    };

    inline thread_local SnapshotPipelineInstrumentation snapshotPipelineCounters;

    inline void resetSnapshotPipelineInstrumentation() noexcept {
        snapshotPipelineCounters = {};
    }

    [[nodiscard]] inline SnapshotPipelineInstrumentation snapshotPipelineInstrumentation() noexcept {
        return snapshotPipelineCounters;
    }

    inline void recordLegacyStateBuild() noexcept {
        ++snapshotPipelineCounters.legacyStateBuilds;
    }

    inline void recordExpandedStateBuild() noexcept {
        ++snapshotPipelineCounters.expandedStateBuilds;
    }

    inline void recordFilteredProjection() noexcept {
        ++snapshotPipelineCounters.filteredProjections;
    }

    inline void recordPassThroughProjection() noexcept {
        ++snapshotPipelineCounters.passThroughProjections;
    }

} // namespace ai::openai::codex::frontend::internal::model::detail

#endif // AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_SNAPSHOTPIPELINEINSTRUMENTATION_H
