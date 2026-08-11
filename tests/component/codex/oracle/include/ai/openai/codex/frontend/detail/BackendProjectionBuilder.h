/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_DETAIL_BACKENDPROJECTIONBUILDER_H
#define AI_OPENAI_CODEX_FRONTEND_DETAIL_BACKENDPROJECTIONBUILDER_H

#include "ai/openai/codex/backend/Snapshot.h"
#include "ai/openai/codex/frontend/detail/FrontendProjection.h"

#include <optional>
#include <string>

namespace ai::openai::codex::frontend::detail {

    inline constexpr SequenceNumber CanonicalSequencePlaceholder{1};

    // Builds the complete safe A1.6b domain view consumed by the generated
    // ExpandedBackendSnapshotState schema. The returned object has no protocol
    // envelope; FrontendProjection selects and wraps it for each connection.
    [[nodiscard]] Json threadListProjection(const backend::ThreadListSnapshot& threadList) noexcept;

    // Resolves the exact production backend-to-frontend item-kind mapping used
    // by every expanded item projection. This detail-only seam also lets
    // currentness tests prove the reviewed mapping without duplicating it.
    [[nodiscard]] std::optional<ThreadItemKind> expandedItemKind(const backend::ItemSnapshot& item) noexcept;

    [[nodiscard]] Json expandedSnapshotState(const backend::Snapshot& snapshot) noexcept;

    [[nodiscard]] CanonicalSnapshotRecord makeCanonicalSnapshotRecord(Json legacyState,
                                                                      const backend::Snapshot& snapshot,
                                                                      SequenceNumber sequence = CanonicalSequencePlaceholder) noexcept;

    // Converts one pre-A1.7 normalized dirty event into a canonical occurrence.
    // codex.extension events are joined to the generated 68-notification
    // projection table. The caller replaces the placeholder with the one
    // sequence allocated when this occurrence enters the shared journal.
    [[nodiscard]] CanonicalEventRecord makeCanonicalEventRecord(std::string legacyType,
                                                                Json legacyData,
                                                                const backend::Snapshot& snapshot,
                                                                SequenceNumber sequence = CanonicalSequencePlaceholder) noexcept;

    void assignCanonicalSequence(CanonicalSnapshotRecord& record, SequenceNumber sequence) noexcept;
    void assignCanonicalSequence(CanonicalEventRecord& record, SequenceNumber sequence) noexcept;

} // namespace ai::openai::codex::frontend::detail

#endif // AI_OPENAI_CODEX_FRONTEND_DETAIL_BACKENDPROJECTIONBUILDER_H
