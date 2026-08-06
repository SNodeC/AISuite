/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/ReferenceInvocationPolicy.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace apps::codex_backend {

    namespace {

        namespace frontend = ai::openai::codex::frontend;

        bool pathIsWithin(const std::filesystem::path& path, const std::filesystem::path& root) noexcept {
            auto pathPart = path.begin();
            for (auto rootPart = root.begin(); rootPart != root.end(); ++rootPart, ++pathPart) {
                if (pathPart == path.end() || *pathPart != *rootPart) {
                    return false;
                }
            }
            return true;
        }

        void validateAllowlistEntry(std::string_view entry, std::string_view description) {
            if (entry.empty() || entry.find('\0') != std::string_view::npos || entry == "*") {
                throw std::invalid_argument(std::string(description) + " contains an invalid allowlist entry");
            }
        }

        void normalizeAllowlist(std::vector<std::string>& values, std::string_view description) {
            for (const std::string& value : values) {
                validateAllowlistEntry(value, description);
            }
            std::ranges::sort(values);
            values.erase(std::unique(values.begin(), values.end()), values.end());
        }

        bool hasExactValue(const std::vector<std::string>& values, const std::string& candidate) noexcept {
            return std::ranges::binary_search(values, candidate);
        }

        bool requiredString(const frontend::Json& value, std::string_view field) noexcept {
            const auto member = value.find(field);
            return member != value.end() && member->is_string();
        }

        bool optionalString(const frontend::Json& value, std::string_view field) noexcept {
            const auto member = value.find(field);
            return member == value.end() || member->is_null() || member->is_string();
        }

        bool optionalBoolean(const frontend::Json& value, std::string_view field) noexcept {
            const auto member = value.find(field);
            return member == value.end() || member->is_boolean();
        }

        bool unsigned16(const frontend::Json& value) noexcept {
            if (!value.is_number_integer()) {
                return false;
            }
            if (value.is_number_unsigned()) {
                return value.get<std::uint64_t>() <= std::numeric_limits<std::uint16_t>::max();
            }
            const std::int64_t signedValue = value.get<std::int64_t>();
            return signedValue >= 0 && signedValue <= std::numeric_limits<std::uint16_t>::max();
        }

    } // namespace

    ReferenceInvocationPolicy::ReferenceInvocationPolicy(ReferenceInvocationPolicyOptions options)
        : commands(std::move(options.commands)) {
        roots.reserve(options.filesystemRoots.size());
        for (const std::filesystem::path& configuredRoot : options.filesystemRoots) {
            if (configuredRoot.empty() || !configuredRoot.is_absolute()) {
                throw std::invalid_argument("filesystem policy roots must be absolute");
            }

            std::error_code error;
            std::filesystem::path canonicalRoot = std::filesystem::canonical(configuredRoot, error);
            if (error || canonicalRoot.empty() || !std::filesystem::is_directory(canonicalRoot, error) || error) {
                throw std::invalid_argument("filesystem policy roots must identify existing directories");
            }
            canonicalRoot = canonicalRoot.lexically_normal();
            if (canonicalRoot == canonicalRoot.root_path()) {
                throw std::invalid_argument("filesystem policy roots may not grant an entire filesystem root");
            }
            roots.push_back(std::move(canonicalRoot));
        }

        std::ranges::sort(roots);
        roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
        normalizeAllowlist(commands.executables, "command executable allowlist");
        normalizeAllowlist(commands.shellCommands, "shell command allowlist");
    }

    bool ReferenceInvocationPolicy::allowsFilesystemRead(std::string_view method,
                                                         const frontend::Json& validatedParameters) const noexcept {
        try {
            if (!validatedParameters.is_object()) {
                return false;
            }
            if (method == "fs.unwatch") {
                // The policy validates the safe identifier shape. BackendCore
                // remains authoritative for matching it to a current watch in
                // the single shared provider context.
                return requiredString(validatedParameters, "watchId");
            }
            if (method == "fuzzyFileSearch") {
                const auto rootsValue = validatedParameters.find("roots");
                return rootsValue != validatedParameters.end() && allowsPathArray(*rootsValue, true);
            }
            if (method == "fs.getMetadata" || method == "fs.readDirectory" || method == "fs.readFile" || method == "fs.watch") {
                const auto path = validatedParameters.find("path");
                return path != validatedParameters.end() && allowsPath(*path);
            }
        } catch (...) {
            return false;
        }
        return false;
    }

    bool ReferenceInvocationPolicy::allowsFilesystemWrite(std::string_view method,
                                                          const frontend::Json& validatedParameters) const noexcept {
        try {
            if (!validatedParameters.is_object()) {
                return false;
            }
            if (method == "fs.copy") {
                const auto source = validatedParameters.find("sourcePath");
                const auto destination = validatedParameters.find("destinationPath");
                return source != validatedParameters.end() && destination != validatedParameters.end() && allowsPath(*source) &&
                       allowsPath(*destination);
            }
            if (method == "fs.createDirectory" || method == "fs.remove" || method == "fs.writeFile") {
                const auto path = validatedParameters.find("path");
                return path != validatedParameters.end() && allowsPath(*path);
            }
        } catch (...) {
            return false;
        }
        return false;
    }

    bool ReferenceInvocationPolicy::allowsCommandExecution(std::string_view method,
                                                           const frontend::Json& validatedParameters) const noexcept {
        try {
            if (!validatedParameters.is_object()) {
                return false;
            }

            if (method == "command.exec") {
                const auto command = validatedParameters.find("command");
                if (command == validatedParameters.end() || !command->is_array() || command->empty()) {
                    return false;
                }
                if (!std::ranges::all_of(*command, [](const frontend::Json& argument) {
                        return argument.is_string();
                    })) {
                    return false;
                }
                const std::string& executable = command->front().get_ref<const std::string&>();
                if (!hasExactValue(commands.executables, executable)) {
                    return false;
                }

                const auto cwd = validatedParameters.find("cwd");
                if (cwd != validatedParameters.end() && !cwd->is_null() && !allowsPath(*cwd)) {
                    return false;
                }

                const auto sandbox = validatedParameters.find("sandboxPolicy");
                if (sandbox == validatedParameters.end() || sandbox->is_null()) {
                    return true;
                }
                if (!sandbox->is_object() || !requiredString(*sandbox, "type")) {
                    return false;
                }
                const std::string& type = sandbox->at("type").get_ref<const std::string&>();
                if (type != "dangerFullAccess" && type != "readOnly" && type != "externalSandbox" && type != "workspaceWrite") {
                    return false;
                }
                const auto writableRoots = sandbox->find("writableRoots");
                return writableRoots == sandbox->end() || allowsPathArray(*writableRoots, false);
            }

            if (method == "command.exec.resize") {
                // Process identity and generation correlation remain owned by
                // BackendCore; this policy only admits schema-valid follow-up
                // parameters after the command-execution deployment gate.
                const auto size = validatedParameters.find("size");
                return requiredString(validatedParameters, "processId") && size != validatedParameters.end() && size->is_object() &&
                       size->contains("cols") && size->contains("rows") && unsigned16(size->at("cols")) && unsigned16(size->at("rows"));
            }
            if (method == "command.exec.terminate") {
                return requiredString(validatedParameters, "processId");
            }
            if (method == "command.exec.write") {
                return requiredString(validatedParameters, "processId") && optionalString(validatedParameters, "deltaBase64") &&
                       optionalBoolean(validatedParameters, "closeStdin");
            }
            if (method == "thread.shellCommand") {
                const auto command = validatedParameters.find("command");
                return requiredString(validatedParameters, "threadId") && command != validatedParameters.end() && command->is_string() &&
                       hasExactValue(commands.shellCommands, command->get_ref<const std::string&>());
            }
        } catch (...) {
            return false;
        }
        return false;
    }

    const std::vector<std::filesystem::path>& ReferenceInvocationPolicy::filesystemRoots() const noexcept {
        return roots;
    }

    const ReferenceCommandAllowlist& ReferenceInvocationPolicy::commandAllowlist() const noexcept {
        return commands;
    }

    bool ReferenceInvocationPolicy::allowsPath(const frontend::Json& value) const noexcept {
        try {
            if (!value.is_string() || roots.empty()) {
                return false;
            }
            const std::filesystem::path candidate(value.get_ref<const std::string&>());
            if (candidate.empty() || !candidate.is_absolute()) {
                return false;
            }
            std::error_code error;
            const std::filesystem::path canonicalCandidate = std::filesystem::weakly_canonical(candidate, error).lexically_normal();
            if (error || canonicalCandidate.empty()) {
                return false;
            }
            return std::ranges::any_of(roots, [&canonicalCandidate](const std::filesystem::path& root) {
                return pathIsWithin(canonicalCandidate, root);
            });
        } catch (...) {
            return false;
        }
    }

    bool ReferenceInvocationPolicy::allowsPathArray(const frontend::Json& value, bool requireNonempty) const noexcept {
        try {
            if (!value.is_array() || (requireNonempty && value.empty())) {
                return false;
            }
            return std::ranges::all_of(value, [this](const frontend::Json& path) {
                return allowsPath(path);
            });
        } catch (...) {
            return false;
        }
    }

} // namespace apps::codex_backend
