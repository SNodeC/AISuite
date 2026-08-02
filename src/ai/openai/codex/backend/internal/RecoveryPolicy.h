/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_BACKEND_INTERNAL_RECOVERYPOLICY_H
#define AI_OPENAI_CODEX_BACKEND_INTERNAL_RECOVERYPOLICY_H

#include "ai/openai/codex/backend/BackendCore.h"

#include <optional>

namespace ai::openai::codex::backend::detail {

    inline bool
    isAutomaticRecoveryEligible(const ProviderState& provider, const RecoveryOptions& options, const std::optional<Error>& error) noexcept {
        if (!provider.desiredRunning || !options.enabled || !error) {
            return false;
        }
        if (error->category != Error::Category::Transport && error->category != Error::Category::Process) {
            return false;
        }
        return options.maximumAttempts == 0 || provider.recovery.attempts < options.maximumAttempts;
    }

} // namespace ai::openai::codex::backend::detail

#endif // AI_OPENAI_CODEX_BACKEND_INTERNAL_RECOVERYPOLICY_H
