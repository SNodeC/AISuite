/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_PROJECTION_H
#define AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_PROJECTION_H

#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/internal/model/Occurrence.h"

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ai::openai::codex::frontend::internal::model {

    struct ProjectionContext {
        FrontendPrincipal principal;
        std::vector<FrontendCapability> selectedCapabilities;
        FrontendPeerContext peer;
        std::optional<ProjectionStamp> continuityFingerprint;
        bool controllerOwned = false;

        bool operator==(const ProjectionContext&) const = default;
    };

    enum class ProjectionAction { Omit, Redact, Truncate, Unavailable };

    struct ProjectionRule {
        std::string jsonPointer;
        std::vector<FrontendScope> requiredScopes;
        std::optional<FrontendCapability> requiredCapability;
        ProjectionAction action = ProjectionAction::Omit;
        std::size_t maximumStringBytes = 0;

        bool operator==(const ProjectionRule&) const = default;
    };

    enum class ProjectionErrorCode { InvalidRule, InvalidValue, UnsafeResult, MissingGeneratedAuthority };

    struct ProjectionError {
        ProjectionErrorCode code = ProjectionErrorCode::InvalidValue;
        std::string path;
        std::string message;

        bool operator==(const ProjectionError&) const = default;
    };

    template <typename Value>
    class ProjectionOutcome {
    public:
        ProjectionOutcome(Value value)
            : result(std::move(value)) {
        }

        ProjectionOutcome(ProjectionError error)
            : result(std::move(error)) {
        }

        [[nodiscard]] bool hasValue() const noexcept {
            return std::holds_alternative<Value>(result);
        }

        explicit operator bool() const noexcept {
            return hasValue();
        }

        [[nodiscard]] const Value& value() const& {
            return std::get<Value>(result);
        }

        [[nodiscard]] Value&& value() && {
            return std::get<Value>(std::move(result));
        }

        [[nodiscard]] const ProjectionError& error() const& {
            return std::get<ProjectionError>(result);
        }

    private:
        std::variant<Value, ProjectionError> result;
    };

    struct ProjectedDetail {
        SafeDetail value;
        ProjectionMetadata metadata;

        bool operator==(const ProjectedDetail&) const = default;
    };

    class ProjectionAuthority {
    public:
        [[nodiscard]] ProjectionOutcome<CanonicalSnapshot>
        projectSnapshot(const CanonicalSnapshot& snapshot, const ProjectionContext& context) const noexcept;

        [[nodiscard]] ProjectionOutcome<std::optional<CanonicalOccurrence>>
        projectOccurrence(const CanonicalOccurrence& occurrence, const ProjectionContext& context) const noexcept;

        [[nodiscard]] ProjectionOutcome<ProjectedDetail>
        projectDetail(const SafeDetail& detail,
                      const ProjectionContext& context,
                      std::span<const ProjectionRule> rules) const noexcept;

        [[nodiscard]] bool
        methodAllowed(const ProjectionContext& context, generated::MethodId method, bool providerReady) const noexcept;
    };

} // namespace ai::openai::codex::frontend::internal::model

#endif // AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_PROJECTION_H
