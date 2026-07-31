/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * Adapted from SNode.C's logging API surface policy.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "tests/policy/support/AISuiteSourcePolicyTestRoot.h"
#include "tests/policy/support/CxxSourceScanner.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

    constexpr std::array<std::string_view, 2> kPolicyPaths = {
        "src/ai/openai/codex/backend/BackendEvent.h",
        "src/ai/openai/codex/backend/BackendState.h",
    };

    constexpr std::array<std::string_view, 4> kForbiddenIdentifiers = {
        "lifecycleStart",
        "creationLogged",
        "lifecycleStarted",
        "lifecycleTerminalLogged",
    };

    void report(std::string_view detail) {
        std::cerr << "CodexPolicyLoggingApiSurfaceMismatch: " << detail << '\n';
    }

} // namespace

int main(int argc, char* argv[]) {
    namespace cxx = aisuite::source_policy::cxx;

    if (!cxx::scannerSelfTest()) {
        report("token-aware scanner self-test failed");
        return 1;
    }

    const std::filesystem::path root = aisuite::source_policy::projectRoot(argc, argv);
    if (root.empty()) {
        report("unable to locate the AISuite source root");
        return 1;
    }

    bool ok = true;
    for (const std::string_view relativePath : kPolicyPaths) {
        const std::filesystem::path path = root / relativePath;
        const std::string source = aisuite::source_policy::readFile(path);
        if (source.empty()) {
            report("unable to read required policy input " + path.generic_string());
            ok = false;
            continue;
        }

        const std::vector<cxx::Token> tokens = cxx::tokenize(source);
        for (const std::string_view identifier : kForbiddenIdentifiers) {
            if (cxx::tokenCount(tokens, identifier) != 0) {
                report("forbidden logging lifecycle identifier " + std::string(identifier) + " appears in " + std::string(relativePath));
                ok = false;
            }
        }
    }

    return ok ? 0 : 1;
}
