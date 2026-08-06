/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_DETAIL_PROVIDERRESULTPROJECTION_H
#define AI_OPENAI_CODEX_FRONTEND_DETAIL_PROVIDERRESULTPROJECTION_H

#include "ai/openai/codex/backend/BackendCommand.h"
#include "ai/openai/codex/frontend/GeneratedProtocol.h"

#include <cstddef>
#include <nlohmann/json.hpp>
#include <span>
#include <string_view>

namespace ai::openai::codex::frontend::detail {

    enum class ProviderResultProjectionStatus {
        Success,
        NotProviderResult,
        ResultTypeMismatch,
        LegacyProjectionRequired,
        InvalidResult,
        ResultTooLarge
    };

    struct ProviderResultProjection {
        ProviderResultProjectionStatus status = ProviderResultProjectionStatus::InvalidResult;
        nlohmann::json value = nlohmann::json::object();

        [[nodiscard]] bool hasValue() const noexcept {
            return status == ProviderResultProjectionStatus::Success;
        }
    };

    inline constexpr std::size_t ProviderResultAlternativeCount = 65;

    [[nodiscard]] std::span<const std::string_view, ProviderResultAlternativeCount> providerResultTypeNames() noexcept;

    [[nodiscard]] std::string_view providerResultTypeName(const backend::CommandValue& value) noexcept;

    [[nodiscard]] bool requiresLegacyProviderResultProjection(generated::MethodId method) noexcept;

    // Converts an exact BackendCore provider result into its method-tagged,
    // generated-schema-validated frontend value.  The six original provider
    // methods deliberately remain on FrontendService's byte-compatible safe
    // snapshot projection path.
    [[nodiscard]] ProviderResultProjection
    projectProviderResult(generated::MethodId method, const backend::CommandValue& value, std::size_t maximumSerializedBytes) noexcept;

} // namespace ai::openai::codex::frontend::detail

#endif // AI_OPENAI_CODEX_FRONTEND_DETAIL_PROVIDERRESULTPROJECTION_H
