/*
 * AISuite - Reusable AI integrations based on SNode.C
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * Public-header guard semantics adapted from SNode.C's Codex public-header
 * policy test.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "../support/AISuiteSourcePolicyTestRoot.h"
#include "../support/CxxSourceScanner.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    constexpr std::string_view kInventoryDiagnostic = "CodexPolicyPublicHeaderInventoryMismatch";
    constexpr std::string_view kGuardDiagnostic = "CodexPolicyHeaderGuardMismatch";
    constexpr std::string_view kCodexInstallRoot = "aisuite/ai/openai/codex";

    struct Component {
        std::string name;
        std::filesystem::path cmakePath;
        std::string variable;
        std::string prefix;
        std::size_t expectedCount;
        std::vector<std::string> headers;
    };

    std::string trim(std::string value) {
        const auto nonspace = [](const unsigned char character) {
            return std::isspace(character) == 0;
        };
        const auto begin = std::find_if(value.begin(), value.end(), nonspace);
        const auto end = std::find_if(value.rbegin(), value.rend(), nonspace).base();
        return begin < end ? std::string(begin, end) : std::string{};
    }

    std::string withoutCMakeComments(const std::string_view source) {
        std::istringstream input{std::string(source)};
        std::ostringstream output;
        std::string line;
        while (std::getline(input, line)) {
            bool quoted = false;
            std::size_t comment = std::string::npos;
            for (std::size_t index = 0; index < line.size(); ++index) {
                if (line[index] == '"' && (index == 0 || line[index - 1] != '\\')) {
                    quoted = !quoted;
                } else if (line[index] == '#' && !quoted) {
                    comment = index;
                    break;
                }
            }
            output << line.substr(0, comment) << '\n';
        }
        return output.str();
    }

    std::vector<std::string> cmakeTokens(const std::string_view fragment) {
        const std::string uncommented = withoutCMakeComments(fragment);
        std::vector<std::string> tokens;
        std::string current;
        bool quoted = false;
        for (const char character : uncommented) {
            if (character == '"') {
                quoted = !quoted;
            } else if (!quoted && (std::isspace(static_cast<unsigned char>(character)) != 0 || character == ';')) {
                if (!current.empty()) {
                    tokens.push_back(std::move(current));
                    current.clear();
                }
            } else {
                current.push_back(character);
            }
        }
        if (!current.empty()) {
            tokens.push_back(std::move(current));
        }
        return tokens;
    }

    bool isHeader(const std::filesystem::path& path) {
        const std::string extension = path.extension().string();
        return extension == ".h" || extension == ".hh" || extension == ".hpp" || extension == ".hxx";
    }

    bool isSafePublicPath(const std::filesystem::path& path) {
        if (path.empty() || path.is_absolute()) {
            return false;
        }
        for (const auto& part : path) {
            if (part == ".." || part == "detail" || part == "private") {
                return false;
            }
        }
        return isHeader(path);
    }

    bool extractAuthority(const std::filesystem::path& root, Component& component) {
        const std::string source = withoutCMakeComments(aisuite::source_policy::readFile(root / component.cmakePath));
        if (source.empty()) {
            return aisuite::source_policy::diagnostic(kInventoryDiagnostic, "cannot read " + component.cmakePath.generic_string());
        }
        const std::regex expression("set\\s*\\(\\s*" + component.variable + "\\s+([^\\)]*)\\)", std::regex::ECMAScript);
        std::smatch match;
        if (!std::regex_search(source, match, expression) || match.size() != 2) {
            return aisuite::source_policy::diagnostic(kInventoryDiagnostic, "cannot derive CMake authority " + component.variable);
        }

        const std::filesystem::path codexSource = root / "src/ai/openai/codex";
        for (const std::string& token : cmakeTokens(match[1].str())) {
            const std::filesystem::path relative = std::filesystem::path(component.prefix) / token;
            if (!isSafePublicPath(relative)) {
                return aisuite::source_policy::diagnostic(kInventoryDiagnostic,
                                                          component.variable + " contains non-public header " + relative.generic_string());
            }
            if (!std::filesystem::is_regular_file(codexSource / relative)) {
                return aisuite::source_policy::diagnostic(kInventoryDiagnostic,
                                                          component.variable + " names missing header " + relative.generic_string());
            }
            component.headers.push_back(relative.lexically_normal().generic_string());
        }
        if (component.headers.size() != component.expectedCount) {
            return aisuite::source_policy::diagnostic(kInventoryDiagnostic,
                                                      component.name + " public-header count is " +
                                                          std::to_string(component.headers.size()) + ", expected " +
                                                          std::to_string(component.expectedCount));
        }
        return true;
    }

    bool hasDuplicates(std::vector<std::string> values, std::string& duplicate) {
        std::sort(values.begin(), values.end());
        const auto repeated = std::adjacent_find(values.begin(), values.end());
        if (repeated == values.end()) {
            return false;
        }
        duplicate = *repeated;
        return true;
    }

    std::vector<std::string>
    expandInstallToken(const std::string& token, const std::map<std::string, std::vector<std::string>>& variables, bool& valid) {
        if (!token.starts_with("${") || !token.ends_with('}')) {
            return {token};
        }
        const std::string variable = token.substr(2, token.size() - 3);
        const auto found = variables.find(variable);
        if (found == variables.end()) {
            aisuite::source_policy::diagnostic(kInventoryDiagnostic, "unresolved install(FILES) variable " + variable);
            valid = false;
            return {};
        }
        return found->second;
    }

    std::vector<std::string> installedHeaders(const std::filesystem::path& root, const std::vector<Component>& components, bool& valid) {
        std::map<std::string, std::vector<std::string>> variables;
        for (const Component& component : components) {
            std::vector<std::string> local;
            local.reserve(component.headers.size());
            for (const std::string& header : component.headers) {
                const std::filesystem::path path = header;
                if (component.prefix.empty()) {
                    local.push_back(path.generic_string());
                } else {
                    local.push_back(path.lexically_relative(component.prefix).generic_string());
                }
            }
            variables.emplace(component.variable, std::move(local));
        }

        std::vector<std::string> installed;
        const std::regex installExpression("install\\s*\\(\\s*FILES\\s+([^\\)]*)\\)", std::regex::ECMAScript);
        for (const Component& component : components) {
            const std::filesystem::path cmakeFile = root / component.cmakePath;
            const std::string source = withoutCMakeComments(aisuite::source_policy::readFile(cmakeFile));
            for (std::sregex_iterator match(source.begin(), source.end(), installExpression), end; match != end; ++match) {
                const std::vector<std::string> tokens = cmakeTokens((*match)[1].str());
                const auto destination = std::find(tokens.begin(), tokens.end(), "DESTINATION");
                if (destination == tokens.end() || std::next(destination) == tokens.end()) {
                    aisuite::source_policy::diagnostic(kInventoryDiagnostic,
                                                       "install(FILES) lacks DESTINATION in " + component.cmakePath.generic_string());
                    valid = false;
                    continue;
                }
                const std::string destinationValue = *std::next(destination);
                const std::size_t rootPosition = destinationValue.find(kCodexInstallRoot);
                if (rootPosition == std::string::npos) {
                    aisuite::source_policy::diagnostic(kInventoryDiagnostic, "Codex install(FILES) has a non-Codex destination");
                    valid = false;
                    continue;
                }
                std::filesystem::path destinationSuffix = destinationValue.substr(rootPosition + kCodexInstallRoot.size());
                if (destinationSuffix.is_absolute()) {
                    destinationSuffix = destinationSuffix.relative_path();
                }
                for (auto token = tokens.begin(); token != destination; ++token) {
                    for (const std::string& expanded : expandInstallToken(*token, variables, valid)) {
                        const std::filesystem::path sourceHeader = expanded;
                        if (!isHeader(sourceHeader)) {
                            aisuite::source_policy::diagnostic(kInventoryDiagnostic, "unexpected install(FILES) entry " + expanded);
                            valid = false;
                            continue;
                        }
                        if (!std::filesystem::is_regular_file(cmakeFile.parent_path() / sourceHeader)) {
                            aisuite::source_policy::diagnostic(kInventoryDiagnostic, "install(FILES) source is missing: " + expanded);
                            valid = false;
                        }
                        const std::filesystem::path installedPath = (destinationSuffix / sourceHeader.filename()).lexically_normal();
                        if (!isSafePublicPath(installedPath)) {
                            aisuite::source_policy::diagnostic(
                                kInventoryDiagnostic, "private/detail header would be installed: " + installedPath.generic_string());
                            valid = false;
                        }
                        installed.push_back(installedPath.generic_string());
                    }
                }
            }
        }
        return installed;
    }

    std::string expectedGuard(const std::string_view relativePath) {
        std::string guard = "ai/openai/codex/" + std::string(relativePath);
        for (char& character : guard) {
            if (std::isalnum(static_cast<unsigned char>(character)) != 0) {
                character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
            } else {
                character = '_';
            }
        }
        return guard;
    }

    struct Directive {
        std::string name;
        std::string argument;
    };

    Directive directive(const std::string_view line) {
        std::string value = trim(std::string(line));
        if (value.empty() || value.front() != '#') {
            return {};
        }
        value = trim(value.substr(1));
        const std::size_t split = value.find_first_of(" \t");
        if (split == std::string::npos) {
            return {value, {}};
        }
        return {value.substr(0, split), trim(value.substr(split + 1))};
    }

    bool checkGuard(const std::filesystem::path& root, const std::string& relativePath) {
        const std::filesystem::path sourcePath = root / "src/ai/openai/codex" / relativePath;
        const std::string source = aisuite::source_policy::readFile(sourcePath);
        const std::string guard = expectedGuard(relativePath);
        const std::string masked = aisuite::source_policy::cxx::maskCommentsAndLiterals(source);
        std::istringstream lines(masked);
        std::string line;
        std::size_t lineNumber = 0;
        std::size_t ifndefCount = 0;
        std::size_t defineCount = 0;
        std::size_t endifCount = 0;
        std::size_t ifndefLine = 0;
        std::size_t defineLine = 0;
        std::size_t endifLine = 0;
        bool pragmaOnce = false;
        while (std::getline(lines, line)) {
            ++lineNumber;
            const Directive current = directive(line);
            if (current.name == "ifndef") {
                ++ifndefCount;
                if (current.argument == guard) {
                    ifndefLine = lineNumber;
                }
            } else if (current.name == "define" && current.argument == guard) {
                ++defineCount;
                defineLine = lineNumber;
            } else if (current.name == "endif") {
                ++endifCount;
                endifLine = lineNumber;
            } else if (current.name == "pragma" && current.argument == "once") {
                pragmaOnce = true;
            }
        }
        const std::string closing = "#endif // " + guard;
        const bool valid = !source.empty() && ifndefCount == 1 && defineCount == 1 && endifCount == 1 &&
                           aisuite::source_policy::occurrenceCount(source, closing) == 1 && ifndefLine != 0 && ifndefLine < defineLine &&
                           defineLine < endifLine && !pragmaOnce;
        if (!valid) {
            return aisuite::source_policy::diagnostic(
                kGuardDiagnostic, relativePath + " must use exactly one ordered " + guard + " guard and no #pragma once");
        }
        return true;
    }
} // namespace

int main(const int argc, char* argv[]) {
    const std::filesystem::path root = aisuite::source_policy::projectRoot(argc, argv);
    if (root.empty()) {
        aisuite::source_policy::diagnostic(kInventoryDiagnostic, "AISuite source root is unavailable");
        return 1;
    }

    std::vector<Component> components = {
        {"main", "src/ai/openai/codex/CMakeLists.txt", "AI_OPENAI_CODEX_PUBLIC_H", "", 29, {}},
        {"backend", "src/ai/openai/codex/backend/CMakeLists.txt", "AI_OPENAI_CODEX_BACKEND_PUBLIC_H", "backend", 7, {}},
        {"frontend", "src/ai/openai/codex/frontend/CMakeLists.txt", "AI_OPENAI_CODEX_FRONTEND_PUBLIC_H", "frontend", 9, {}},
    };
    bool valid = true;
    for (Component& component : components) {
        valid = extractAuthority(root, component) && valid;
    }
    if (!valid) {
        return 1;
    }

    const std::string eventJournalHeader = aisuite::source_policy::cxx::maskCommentsAndLiterals(
        aisuite::source_policy::readFile(root / "src/ai/openai/codex/frontend/EventJournal.h"));
    for (const std::string_view legacyType : {"JournalAppendResult", "JournalReplayResult"}) {
        const std::regex expression("(^|[^A-Za-z0-9_])" + std::string(legacyType) + "([^A-Za-z0-9_]|$)", std::regex::ECMAScript);
        if (std::regex_search(eventJournalHeader, expression)) {
            valid = aisuite::source_policy::diagnostic(
                kInventoryDiagnostic, "frontend/EventJournal.h exposes legacy second-authority type " + std::string(legacyType));
        }
    }
    for (const std::string_view legacyMethod : {"append", "replayAfter", "retainedEvents"}) {
        const std::regex expression("(^|[^A-Za-z0-9_])" + std::string(legacyMethod) + "\\s*\\(", std::regex::ECMAScript);
        if (std::regex_search(eventJournalHeader, expression)) {
            valid = aisuite::source_policy::diagnostic(
                kInventoryDiagnostic, "frontend/EventJournal.h exposes legacy second-authority method " + std::string(legacyMethod));
        }
    }
    if (!valid) {
        return 1;
    }

    constexpr std::array<std::string_view, 9> expectedFrontendHeaders = {{
        "frontend/Codec.h",
        "frontend/EventCoalescer.h",
        "frontend/EventJournal.h",
        "frontend/FrontendService.h",
        "frontend/GeneratedProtocol.h",
        "frontend/Messages.h",
        "frontend/Protocol.h",
        "frontend/Security.h",
        "frontend/UpdateBatch.h",
    }};
    std::vector<std::string> actualFrontendHeaders = components[2].headers;
    std::sort(actualFrontendHeaders.begin(), actualFrontendHeaders.end());
    if (!std::equal(
            actualFrontendHeaders.begin(), actualFrontendHeaders.end(), expectedFrontendHeaders.begin(), expectedFrontendHeaders.end())) {
        valid = aisuite::source_policy::diagnostic(
            kInventoryDiagnostic,
            "frontend public-header inventory must be exactly the nine A1.7b headers with FrontendService and without BackendAdapter");
    }

    constexpr std::array<std::string_view, 5> forbiddenPublicSymbols = {{
        "BackendAdapter",
        "BackendAdapterOptions",
        "FrontendClient",
        "OpenAICodexFrontendClient",
        "declaredTransportFamilies",
    }};
    for (const Component& component : components) {
        for (const std::string& header : component.headers) {
            const std::string source = aisuite::source_policy::cxx::maskCommentsAndLiterals(
                aisuite::source_policy::readFile(root / "src/ai/openai/codex" / header));
            for (const std::string_view symbol : forbiddenPublicSymbols) {
                const std::regex expression("(^|[^A-Za-z0-9_])" + std::string(symbol) + "([^A-Za-z0-9_]|$)", std::regex::ECMAScript);
                if (std::regex_search(source, expression)) {
                    valid = aisuite::source_policy::diagnostic(
                        kInventoryDiagnostic, header + " exposes forbidden pre-A1.7b or future-SDK symbol " + std::string(symbol));
                }
            }
        }
    }
    if (!valid) {
        return 1;
    }

    std::vector<std::string> authority;
    for (const Component& component : components) {
        authority.insert(authority.end(), component.headers.begin(), component.headers.end());
    }
    if (authority.size() != 45) {
        valid = aisuite::source_policy::diagnostic(kInventoryDiagnostic, "total public-header count is not 45");
    }
    std::string duplicate;
    if (hasDuplicates(authority, duplicate)) {
        valid = aisuite::source_policy::diagnostic(kInventoryDiagnostic, "duplicate CMake-public header " + duplicate);
    }

    std::vector<std::string> installed = installedHeaders(root, components, valid);
    if (hasDuplicates(installed, duplicate)) {
        valid = aisuite::source_policy::diagnostic(kInventoryDiagnostic, "header is installed more than once: " + duplicate);
    }
    std::sort(authority.begin(), authority.end());
    std::sort(installed.begin(), installed.end());
    if (authority != installed) {
        valid = aisuite::source_policy::diagnostic(kInventoryDiagnostic, "CMake public inventories and install(FILES) declarations differ");
    }
    if (!valid) {
        return 1;
    }

    constexpr std::array<std::pair<std::string_view, std::string_view>, 3> originalGuards = {{
        {"typed/Accounts.h", "AI_OPENAI_CODEX_TYPED_ACCOUNTS_H"},
        {"typed/Models.h", "AI_OPENAI_CODEX_TYPED_MODELS_H"},
        {"typed/Configuration.h", "AI_OPENAI_CODEX_TYPED_CONFIGURATION_H"},
    }};
    for (const auto& [header, guard] : originalGuards) {
        if (expectedGuard(header) != guard) {
            valid = aisuite::source_policy::diagnostic(kGuardDiagnostic, std::string(header) + " original A1.2 guard identity drifted");
        }
    }
    for (const std::string& header : authority) {
        valid = checkGuard(root, header) && valid;
    }
    if (valid) {
        std::cout << "Codex public-header policy verified: main=29 backend=7 frontend=9 total=45\n";
    }
    return valid ? 0 : 1;
}
