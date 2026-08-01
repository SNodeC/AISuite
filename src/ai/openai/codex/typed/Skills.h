/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_TYPED_SKILLS_H
#define AI_OPENAI_CODEX_TYPED_SKILLS_H

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/typed/Results.h"
#include "ai/openai/codex/typed/Types.h"

#include <compare>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ai::openai::codex::typed {

    struct SkillScope {
        std::string value;

        static SkillScope user() {
            return {"user"};
        }

        static SkillScope repo() {
            return {"repo"};
        }

        static SkillScope system() {
            return {"system"};
        }

        static SkillScope admin() {
            return {"admin"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "user" || value == "repo" || value == "system" || value == "admin";
        }

        auto operator<=>(const SkillScope&) const = default;
    };

    struct SkillToolDependency {
        OptionalNullable<std::string> command;
        OptionalNullable<std::string> description;
        OptionalNullable<std::string> transport;
        std::string type;
        OptionalNullable<std::string> url;
        std::string value;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const SkillToolDependency&) const = default;
    };

    struct SkillDependencies {
        std::vector<SkillToolDependency> tools;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const SkillDependencies&) const = default;
    };

    struct SkillErrorInfo {
        std::string message;
        std::string path;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const SkillErrorInfo&) const = default;
    };

    struct SkillInterface {
        OptionalNullable<std::string> brandColor;
        OptionalNullable<std::string> defaultPrompt;
        OptionalNullable<std::string> displayName;
        OptionalNullable<AbsolutePath> iconLarge;
        OptionalNullable<AbsolutePath> iconSmall;
        OptionalNullable<std::string> shortDescription;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const SkillInterface&) const = default;
    };

    struct SkillMetadata {
        OptionalNullable<SkillDependencies> dependencies;
        std::string description;
        bool enabled = false;
        OptionalNullable<SkillInterface> interface;
        std::string name;
        AbsolutePath path;
        SkillScope scope;
        OptionalNullable<std::string> shortDescription;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const SkillMetadata&) const = default;
    };

    struct SkillsListEntry {
        std::string cwd;
        std::vector<SkillErrorInfo> errors;
        std::vector<SkillMetadata> skills;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const SkillsListEntry&) const = default;
    };

    struct SkillsConfigWriteParams {
        bool enabled = false;
        OptionalNullable<std::string> name;
        OptionalNullable<AbsolutePath> path;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const SkillsConfigWriteParams&) const = default;
    };

    struct SkillsConfigWriteResponse {
        bool effectiveEnabled = false;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const SkillsConfigWriteResponse&) const = default;
    };

    struct SkillsExtraRootsSetParams {
        std::vector<AbsolutePath> extraRoots;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const SkillsExtraRootsSetParams&) const = default;
    };

    struct SkillsListParams {
        std::optional<std::vector<std::string>> cwds;
        std::optional<bool> forceReload;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const SkillsListParams&) const = default;
    };

    struct SkillsListResponse {
        std::vector<SkillsListEntry> data;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const SkillsListResponse&) const = default;
    };

    struct SkillsChangedNotification {
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const SkillsChangedNotification&) const = default;
    };

    class Skills {
    public:
        Skills(const Skills&) = delete;
        Skills(Skills&&) = delete;
        Skills& operator=(const Skills&) = delete;
        Skills& operator=(Skills&&) = delete;

        Submission writeConfig(SkillsConfigWriteParams params, CompletionHandler<SkillsConfigWriteResponse> handler);
        Submission setExtraRoots(SkillsExtraRootsSetParams params, DoneHandler handler);
        Submission list(SkillsListParams params, CompletionHandler<SkillsListResponse> handler);
        Submission list(CompletionHandler<SkillsListResponse> handler);

    private:
        friend class ::ai::openai::codex::AppServerClient;

        explicit Skills(AppServerClient::RawProtocol& protocol) noexcept;

        AppServerClient::RawProtocol* protocol;
    };

} // namespace ai::openai::codex::typed

#endif // AI_OPENAI_CODEX_TYPED_SKILLS_H
