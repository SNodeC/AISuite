/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_DETAIL_GENERATEDSCHEMAVALIDATOR_H
#define AI_OPENAI_CODEX_FRONTEND_DETAIL_GENERATEDSCHEMAVALIDATOR_H

#include "ai/openai/codex/frontend/Messages.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace ai::openai::codex::frontend::detail {

    struct SchemaValidationLimits {
        std::size_t maximumDepth = 128;
        std::size_t maximumVisits = 4'000'000;
    };

    struct SchemaValidationStatistics {
        std::size_t visits = 0;
        std::size_t maximumDepthObserved = 0;
        std::size_t referencesResolved = 0;
        std::size_t alternativesEvaluated = 0;
        std::size_t discriminatorFastPaths = 0;
        std::size_t uniqueItemComparisons = 0;
        std::size_t regularExpressionsEvaluated = 0;
        bool complexityRejected = false;
    };

    struct GeneratedSchemaValidation {
        bool valid = true;
        bool missingRequired = false;
        bool internalFailure = false;
        std::string message;
    };

    [[nodiscard]] const Json& generatedProtocolSchema();

    [[nodiscard]] GeneratedSchemaValidation validateGeneratedSchema(const Json& root,
                                                                    std::string_view reference,
                                                                    const Json& value,
                                                                    std::string_view valueName,
                                                                    SchemaValidationLimits limits = {},
                                                                    SchemaValidationStatistics* statistics = nullptr) noexcept;

    // Private test seam for deterministic synthetic-schema validation. This
    // header is neither installed nor part of the exported frontend API.
    [[nodiscard]] GeneratedSchemaValidation validateGeneratedSchemaNodeForTest(const Json& root,
                                                                               const Json& schema,
                                                                               const Json& value,
                                                                               std::string_view valueName,
                                                                               SchemaValidationLimits limits = {},
                                                                               SchemaValidationStatistics* statistics = nullptr) noexcept;

} // namespace ai::openai::codex::frontend::detail

#endif // AI_OPENAI_CODEX_FRONTEND_DETAIL_GENERATEDSCHEMAVALIDATOR_H
