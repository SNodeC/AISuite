/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/stdio/Client.h"
#include "apps/codex-backend/Configuration.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {
    std::string environmentValue(const char* name) {
        const char* value = std::getenv(name);
        return value == nullptr ? std::string{} : std::string(value);
    }
} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    if (argc != 2) {
        result.expectTrue(false, "one stdio environment scenario is required");
        return result.processResult();
    }

    const std::string scenario = argv[1];
    const bool configured = scenario == "configured";
    if (!configured && scenario != "inherited") {
        result.expectTrue(false, "the stdio environment scenario is recognized");
        return result.processResult();
    }

    const std::string parentCodexHome = environmentValue("CODEX_HOME");
    const std::string configuredCodexHome = "/tmp/aisuite-configured-codex-home";
    apps::codex_backend::AppServerProcessConfiguration processConfiguration;
    std::vector<std::string> arguments{"CodexStdioEnvironmentTest"};
    if (configured) {
        arguments.insert(arguments.end(), {"--codex-home", configuredCodexHome});
    }
    std::vector<char*> snodeArguments;
    snodeArguments.reserve(arguments.size());
    for (std::string& argument : arguments) {
        snodeArguments.push_back(argument.data());
    }
    core::SNodeC::init(static_cast<int>(snodeArguments.size()), snodeArguments.data());

    const auto environmentOverrides = processConfiguration.environmentOverrides();
    result.expectTrue(configured
                          ? environmentOverrides == std::vector<std::pair<std::string, std::string>>{{"CODEX_HOME", configuredCodexHome}}
                          : environmentOverrides.empty(),
                      configured ? "--codex-home produces one CODEX_HOME child override"
                                 : "an absent --codex-home produces no child environment overrides");
    result.expectTrue(environmentValue("CODEX_HOME") == parentCodexHome,
                      "reading app-server configuration does not modify the parent environment");

    std::vector<std::string> diagnostics;
    bool ready = false;
    bool failed = false;
    bool timedOut = false;
    ai::openai::codex::typed::InitializeParams initializeParams = apps::codex_backend::appServerInitializeParams();
    result.expectTrue(initializeParams.capabilities.present && initializeParams.capabilities.value &&
                          initializeParams.capabilities.value->experimentalApi == true,
                      "codex-backend enables the app-server experimental API capability");
    ai::openai::codex::stdio::Client client(
        CODEX_FAKE_APP_SERVER, {"environment"}, std::move(initializeParams), environmentOverrides);
    client.setOnDiagnostic([&diagnostics](const ai::openai::codex::Diagnostic& diagnostic) {
        diagnostics.push_back(diagnostic.message);
    });
    client.setOnStateChanged([&](const ai::openai::codex::StateChange& stateChange) {
        if (stateChange.current == ai::openai::codex::State::Ready) {
            ready = true;
            client.stop();
        } else if (stateChange.current == ai::openai::codex::State::Failed) {
            failed = true;
            core::SNodeC::stop();
        } else if (stateChange.current == ai::openai::codex::State::Stopped) {
            core::SNodeC::stop();
        }
    });
    [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
        [&timedOut]() {
            timedOut = true;
            core::SNodeC::stop();
        },
        utils::Timeval({4, 0}));

    client.start();
    const int startResult = core::SNodeC::start(utils::Timeval({5, 0}));
    const std::string expectedChildCodexHome = configured ? configuredCodexHome : parentCodexHome;
    const std::string expectedDiagnostic = "codex-home=" + expectedChildCodexHome;
    const bool childReceivedExpectedValue = std::ranges::find(diagnostics, expectedDiagnostic) != diagnostics.end();
    const bool experimentalApiEnabled = std::ranges::find(diagnostics, "experimental-api=true") != diagnostics.end();

    result.expectTrue(ready && !failed && !timedOut && startResult == 0, scenario + ": the app-server child completes its stdio lifecycle");
    result.expectTrue(childReceivedExpectedValue,
                      configured ? "the spawned child receives CODEX_HOME from --codex-home"
                                 : "without --codex-home the child inherits the parent's CODEX_HOME");
    result.expectTrue(experimentalApiEnabled, "the spawned app-server receives experimentalApi=true on the initialize wire");
    result.expectTrue(environmentValue("CODEX_HOME") == parentCodexHome, "launching the app-server does not modify the parent CODEX_HOME");

    core::SNodeC::free();
    return result.processResult();
}
