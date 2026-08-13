/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_INTERNAL_CLIENT_CANONICALSTATEBUILDER_H
#define AI_OPENAI_CODEX_FRONTEND_INTERNAL_CLIENT_CANONICALSTATEBUILDER_H

#include "ai/openai/codex/frontend/client/State.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace ai::openai::codex::frontend::internal::client {
    struct PublishedState;
}

namespace ai::openai::codex::frontend::client::detail {

    enum class CanonicalStateBuildFailure {
        StateDivergence,
        Capacity,
    };

    // Private cutover adapter. It consumes the typed canonical publication
    // directly; it never encodes a Frontend Protocol Snapshot and never calls
    // a JSON reduction round-trip.
    class CanonicalStateBuilder final {
    public:
        [[nodiscard]] static std::optional<std::shared_ptr<const StateStorage>>
        build(const internal::client::PublishedState& publication,
              std::size_t maximumBytes,
              std::size_t maximumRetainedDiagnostics,
              std::string& error,
              CanonicalStateBuildFailure* failure = nullptr) noexcept;
    };

} // namespace ai::openai::codex::frontend::client::detail

#endif // AI_OPENAI_CODEX_FRONTEND_INTERNAL_CLIENT_CANONICALSTATEBUILDER_H
