/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Security.h"
#include "apps/codex-backend/Configuration.h"
#include "core/SNodeC.h"
#include "support/TestResult.h"

#include <string>
#include <vector>

int main() {
    namespace app = apps::codex_backend;
    namespace frontend = ai::openai::codex::frontend;

    app::ReferenceAuthenticationConfiguration authentication;
    app::FrontendRuntimeConfiguration runtime;
    std::vector<std::string> arguments{
        "CodexFrontendNativeConfigurationCliTest",
        "--frontend-unix-verified-local-trust=false",
        "--frontend-unix-insecure-local-trust=true",
        "--frontend-bearer-token-file=/tmp/aisuite-cli-test.token",
        "--frontend-remote-principal-id=explicit-remote",
        "--frontend-remote-scope-profile=local_trusted",
        "--frontend-allow-insecure-remote=true",
        "--frontend-max-connections=23",
        "--frontend-max-unauthenticated-connections=7",
        "--frontend-maximum-message-bytes=4096",
    };
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }

    core::SNodeC::init(static_cast<int>(argv.size()), argv.data());
    tests::support::TestResult result;
    {
        const app::ReferenceAuthenticationOptions parsed = authentication.options();
        result.expectTrue(!authentication.verifiedLocalTrustEnabled() && authentication.insecureLocalTrustOverride() &&
                              authentication.allowInsecureRemote() && authentication.bearerTokenFile() == "/tmp/aisuite-cli-test.token",
                          "AISuite policy options remain independently configurable");
        result.expectTrue(parsed.remotePrincipalId == "explicit-remote" && parsed.remoteProfile == "local_trusted" &&
                              parsed.remoteScopes.size() == 12,
                          "the configured reference principal uses the complete built-in local_trusted profile");
        frontend::FrontendServiceOptions serviceOptions;
        const auto error = runtime.apply(serviceOptions);
        result.expectTrue(!error && serviceOptions.maxConnections == 23 && serviceOptions.maxUnauthenticatedConnections == 7 &&
                              serviceOptions.maximumInboundMessageBytes == 4096,
                          "effective AISuite policy configuration is applied without listener fields");
    }
    core::SNodeC::free();
    return result.processResult();
}
