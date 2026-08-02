/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_BACKEND_INTERNAL_RETENTIONCAPACITYINSTRUMENTATION_H
#define AI_OPENAI_CODEX_BACKEND_INTERNAL_RETENTIONCAPACITYINSTRUMENTATION_H

#include <cstddef>

namespace ai::openai::codex::backend::detail {

    struct RetentionCapacityInstrumentation {
        std::size_t slowPathEntries = 0;
        std::size_t pendingReferenceBuilds = 0;
    };

    void resetRetentionCapacityInstrumentation() noexcept;
    RetentionCapacityInstrumentation retentionCapacityInstrumentation() noexcept;
    void recordRetentionCapacitySlowPath() noexcept;
    void recordPendingReferenceBuild() noexcept;

} // namespace ai::openai::codex::backend::detail

#endif // AI_OPENAI_CODEX_BACKEND_INTERNAL_RETENTIONCAPACITYINSTRUMENTATION_H
