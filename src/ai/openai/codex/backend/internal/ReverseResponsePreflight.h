/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_BACKEND_INTERNAL_REVERSERESPONSEPREFLIGHT_H
#define AI_OPENAI_CODEX_BACKEND_INTERNAL_REVERSERESPONSEPREFLIGHT_H

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/backend/BackendState.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <utility>

namespace ai::openai::codex::backend::detail {

    enum class ReverseResponsePreflightStatus {
        Submitted,
        SequenceUnavailable,
    };

    template <typename Submit>
    ReverseResponsePreflightStatus
    submitReverseResponseIfSequenceAvailable(const BackendState& state, SendResult& result, Submit&& submit) {
        if (state.sequenceExhausted || state.sequence.value() == std::numeric_limits<std::uint64_t>::max()) {
            return ReverseResponsePreflightStatus::SequenceUnavailable;
        }
        result = std::invoke(std::forward<Submit>(submit));
        return ReverseResponsePreflightStatus::Submitted;
    }

} // namespace ai::openai::codex::backend::detail

#endif // AI_OPENAI_CODEX_BACKEND_INTERNAL_REVERSERESPONSEPREFLIGHT_H
