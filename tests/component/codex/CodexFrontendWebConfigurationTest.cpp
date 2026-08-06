/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/Configuration.h"
#include "core/SNodeC.h"
#include "support/TestResult.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

    class TemporaryDirectory {
    public:
        TemporaryDirectory() {
            std::string pattern = (std::filesystem::temp_directory_path() / "aisuite-web-config-XXXXXX").string();
            if (char* created = ::mkdtemp(pattern.data()); created != nullptr) {
                directory = std::filesystem::canonical(created);
            }
        }

        ~TemporaryDirectory() {
            std::error_code error;
            if (!directory.empty()) {
                std::filesystem::remove_all(directory, error);
            }
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

        std::filesystem::path directory;
    };

} // namespace

int main() {
    namespace app = apps::codex_backend;

    TemporaryDirectory temporary;
    app::FrontendWebConfiguration configuration;
    std::vector<std::string> arguments{
        "CodexFrontendWebConfigurationTest",
        "--frontend-websocket-endpoint=/api/frontend-v1",
        "--frontend-static-root=" + temporary.directory.string(),
        "--frontend-websocket-allowed-origins=https://example.test,http://127.0.0.1:43190",
    };
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }

    core::SNodeC::init(static_cast<int>(argv.size()), argv.data());
    tests::support::TestResult result;
    {
        const app::FrontendWebOptions parsed = configuration.options();
        result.expectTrue(parsed.endpoint == "/api/frontend-v1" && parsed.staticRoot == temporary.directory &&
                              parsed.allowedOrigins == std::vector<std::string>{"https://example.test", "http://127.0.0.1:43190"},
                          "only endpoint, static-root, and Origin application policy is parsed by AISuite");
        result.expectTrue(!parsed.validationError(), "the normalized application web policy is valid");
    }
    core::SNodeC::free();

    app::FrontendWebOptions invalid;
    invalid.endpoint = "/frontend?token=forbidden";
    result.expectTrue(invalid.validationError().has_value(), "query-bearing endpoint aliases remain rejected");
    invalid.endpoint = "/frontend";
    invalid.staticRoot = std::filesystem::path{"/"};
    result.expectTrue(invalid.validationError().has_value(), "a filesystem-wide static root remains rejected");
    invalid.staticRoot.reset();
    invalid.allowedOrigins = {"*"};
    result.expectTrue(invalid.validationError().has_value(), "wildcard Origin policy remains rejected");

    const app::FrontendWebOptions defaults;
    result.expectTrue(defaults.endpoint == "/frontend" && !defaults.staticRoot && defaults.allowedOrigins.empty() &&
                          !defaults.validationError(),
                      "the application web policy defaults to /frontend with no static product or Origin wildcard");
    return result.processResult();
}
