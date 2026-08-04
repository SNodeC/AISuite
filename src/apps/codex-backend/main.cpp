/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/frontend/FrontendService.h"
#include "ai/openai/codex/stdio/Client.h"
#include "apps/codex-backend/CodexFrontendSocketContextFactory.h"
#include "apps/codex-backend/Configuration.h"
#include "apps/codex-backend/ReferenceAuthentication.h"
#include "apps/codex-backend/UnixPeerCredentials.h"
#include "core/SNodeC.h"
#include "net/un/stream/legacy/SocketServer.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <variant>

int main(int argc, char* argv[]) {
    apps::codex_backend::ProviderRecoveryConfiguration recoveryConfiguration;
    apps::codex_backend::ReferenceAuthenticationConfiguration authenticationConfiguration;
    core::SNodeC::init(argc, argv);

    int result = 1;
    {
        std::optional<apps::codex_backend::ProtectedBearerToken> bearerToken;
        const std::string bearerTokenFile = authenticationConfiguration.bearerTokenFile();
        if (!bearerTokenFile.empty()) {
            apps::codex_backend::ProtectedTokenFileResult loadedToken = apps::codex_backend::loadProtectedBearerTokenFile(bearerTokenFile);
            if (auto* error = std::get_if<apps::codex_backend::ProtectedTokenFileError>(&loadedToken)) {
                std::cerr << "codex-backend: " << error->message << '\n';
                core::SNodeC::free();
                return 1;
            }
            bearerToken.emplace(std::move(std::get<apps::codex_backend::ProtectedBearerToken>(loadedToken)));
        }
        apps::codex_backend::ReferenceAuthenticator authenticator(std::move(bearerToken), authenticationConfiguration.options());

        ai::openai::codex::backend::BackendCoreOptions backendOptions;
        backendOptions.recovery = recoveryConfiguration.options();
        ai::openai::codex::backend::BackendCore<ai::openai::codex::stdio::Client> backend(std::move(backendOptions));
        ai::openai::codex::frontend::FrontendServiceOptions frontendOptions;
        frontendOptions.allowVerifiedLocalTrust = authenticationConfiguration.verifiedLocalTrustEnabled();
        frontendOptions.allowInsecureLocalTrust = authenticationConfiguration.insecureLocalTrustOverride();
        if (frontendOptions.allowVerifiedLocalTrust) {
            frontendOptions.trustedLocalUserId = static_cast<std::uint64_t>(::geteuid());
        }
        frontendOptions.authenticator = [&authenticator](const ai::openai::codex::frontend::FrontendPeerContext& peer,
                                                         const ai::openai::codex::frontend::AuthenticationCredential& credential) {
            return authenticator.authenticate(peer, credential);
        };
        if (frontendOptions.allowInsecureLocalTrust) {
            std::cerr << "codex-backend: WARNING: insecure Unix frontend local-trust override is enabled\n";
        }
        ai::openai::codex::frontend::FrontendService frontendService(backend, std::move(frontendOptions));

        auto socketServer = net::un::stream::legacy::Server<apps::codex_backend::CodexFrontendSocketContextFactory>(
            "codex-backend",
            [](net::un::stream::legacy::config::ConfigSocketServer* config) {
                // setSunPath supplies the safe application default while keeping
                // the ordinary SNode.C --sun-path/config-file option authoritative.
                config->Local::setSunPath(apps::codex_backend::defaultSocketPath());
            },
            frontendService);
        socketServer.listen([&backend, &frontendService](const net::un::SocketAddress& address, core::socket::State state) {
            if (state != core::socket::State::OK) {
                std::cerr << "codex-backend: failed to listen on Unix socket " << address.toString() << '\n';
                backend.stop();
                core::SNodeC::stop();
                return;
            }

            const std::string socketPath = address.getSunPath();
            const bool ownerOnly =
                !socketPath.empty() && ::chmod(socketPath.c_str(), S_IRUSR | S_IWUSR) == 0 &&
                apps::codex_backend::verifyUnixListenerPath(socketPath, static_cast<std::uint64_t>(::geteuid())).verified;
            if (!ownerOnly) {
                std::cerr << "codex-backend: failed to secure the Unix frontend listener pathname\n";
                backend.stop();
                core::SNodeC::stop();
                return;
            }
            frontendService.declareTransportFamily(ai::openai::codex::frontend::FrontendTransportKind::Unix);
        });

        backend.start();
        result = core::SNodeC::start();
    }

    // EventLoop::start performs the primary global shutdown. free() is
    // intentionally repeated after the socket server and backend have been
    // destroyed; the SNode.C lifecycle makes this cleanup idempotent.
    core::SNodeC::free();
    return result;
}
