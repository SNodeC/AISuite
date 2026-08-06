/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_REFERENCEINVOCATIONPOLICY_H
#define APPS_CODEX_BACKEND_REFERENCEINVOCATIONPOLICY_H

#include "ai/openai/codex/frontend/Messages.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace apps::codex_backend {

    struct ReferenceCommandAllowlist {
        // command.exec compares argv[0] byte-for-byte with these entries.
        std::vector<std::string> executables;

        // thread.shellCommand compares the complete shell program byte-for-byte
        // with these entries. No tokenization or shell normalization is used.
        std::vector<std::string> shellCommands;
    };

    struct ReferenceInvocationPolicyOptions {
        std::vector<std::filesystem::path> filesystemRoots;
        ReferenceCommandAllowlist commands;
    };

    // Application-private reference deployment policy. FrontendService has
    // already applied schema validation before invoking these checks, but the
    // implementation still fails closed for malformed values and exceptions.
    class ReferenceInvocationPolicy {
    public:
        explicit ReferenceInvocationPolicy(ReferenceInvocationPolicyOptions options);

        [[nodiscard]] bool allowsFilesystemRead(std::string_view method,
                                                const ai::openai::codex::frontend::Json& validatedParameters) const noexcept;
        [[nodiscard]] bool allowsFilesystemWrite(std::string_view method,
                                                 const ai::openai::codex::frontend::Json& validatedParameters) const noexcept;
        [[nodiscard]] bool allowsCommandExecution(std::string_view method,
                                                  const ai::openai::codex::frontend::Json& validatedParameters) const noexcept;

        [[nodiscard]] const std::vector<std::filesystem::path>& filesystemRoots() const noexcept;
        [[nodiscard]] const ReferenceCommandAllowlist& commandAllowlist() const noexcept;

    private:
        [[nodiscard]] bool allowsPath(const ai::openai::codex::frontend::Json& value) const noexcept;
        [[nodiscard]] bool allowsPathArray(const ai::openai::codex::frontend::Json& value, bool requireNonempty) const noexcept;

        std::vector<std::filesystem::path> roots;
        ReferenceCommandAllowlist commands;
    };

} // namespace apps::codex_backend

#endif // APPS_CODEX_BACKEND_REFERENCEINVOCATIONPOLICY_H
