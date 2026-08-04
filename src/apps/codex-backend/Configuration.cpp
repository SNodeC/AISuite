/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/Configuration.h"

#include "utils/Config.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace apps::codex_backend {

    ProviderRecoveryConfiguration::ProviderRecoveryConfiguration() {
        auto& root = utils::Config::configRoot;

        enabledOption = root.setConfigurable(root.addFlag("--provider-recovery-enabled{true}",
                                                          "Automatically recover the Codex provider after transport or process failures",
                                                          "BOOL",
                                                          "true",
                                                          CLI::IsMember({"true", "false"})),
                                             true);
        maximumAttemptsOption = root.setConfigurable(root.addOption("--provider-recovery-maximum-attempts",
                                                                    "Maximum automatic recovery attempts (zero means unlimited)",
                                                                    "UINT",
                                                                    std::uint32_t{0},
                                                                    CLI::TypeValidator<std::uint32_t>()),
                                                     true);
        initialDelayOption = root.setConfigurable(root.addOption("--provider-recovery-initial-delay-ms",
                                                                 "Initial provider recovery delay in milliseconds",
                                                                 "UINT64",
                                                                 std::uint64_t{1000},
                                                                 CLI::TypeValidator<std::uint64_t>()),
                                                  true);
        maximumDelayOption = root.setConfigurable(root.addOption("--provider-recovery-maximum-delay-ms",
                                                                 "Maximum provider recovery delay in milliseconds",
                                                                 "UINT64",
                                                                 std::uint64_t{30000},
                                                                 CLI::TypeValidator<std::uint64_t>()),
                                                  true);
        multiplierOption = root.setConfigurable(root.addOption("--provider-recovery-multiplier",
                                                               "Provider recovery exponential-delay multiplier",
                                                               "UINT",
                                                               std::uint32_t{2},
                                                               CLI::TypeValidator<std::uint32_t>()),
                                                true);
    }

    ai::openai::codex::backend::RecoveryOptions ProviderRecoveryConfiguration::options() const {
        return {.enabled = enabledOption->as<bool>(),
                .maximumAttempts = maximumAttemptsOption->as<std::uint32_t>(),
                .initialDelayMs = initialDelayOption->as<std::uint64_t>(),
                .maximumDelayMs = maximumDelayOption->as<std::uint64_t>(),
                .multiplier = multiplierOption->as<std::uint32_t>()};
    }

    ReferenceAuthenticationConfiguration::ReferenceAuthenticationConfiguration() {
        auto& root = utils::Config::configRoot;

        verifiedLocalTrustOption =
            root.setConfigurable(root.addFlag("--frontend-unix-verified-local-trust{true}",
                                              "Allow owner-only Unix listeners to authenticate verified same-user peers",
                                              "BOOL",
                                              "true",
                                              CLI::IsMember({"true", "false"})),
                                 true);
        insecureLocalTrustOverrideOption =
            root.setConfigurable(root.addFlag("--frontend-unix-insecure-local-trust{false}",
                                              "Explicitly trust Unix peers when OS peer credentials are unavailable (unsafe)",
                                              "BOOL",
                                              "false",
                                              CLI::IsMember({"true", "false"})),
                                 true);
        bearerTokenFileOption = root.setConfigurable(root.addOption("--frontend-bearer-token-file",
                                                                    "Protected file containing the remote frontend bearer token",
                                                                    "PATH",
                                                                    std::string{},
                                                                    CLI::Validator{}),
                                                     true);
        remotePrincipalIdOption = root.setConfigurable(root.addOption("--frontend-remote-principal-id",
                                                                      "Principal identifier assigned after bearer authentication",
                                                                      "ID",
                                                                      std::string{"remote"},
                                                                      CLI::Validator{}),
                                                       true);
        remoteScopeProfileOption = root.setConfigurable(root.addOption("--frontend-remote-scope-profile",
                                                                       "Scope profile assigned after bearer authentication",
                                                                       "PROFILE",
                                                                       std::string{"default_remote"},
                                                                       CLI::IsMember({"default_remote"})),
                                                        true);
    }

    ReferenceAuthenticationOptions ReferenceAuthenticationConfiguration::options() const {
        ReferenceAuthenticationOptions authenticationOptions = defaultReferenceAuthenticationOptions();
        authenticationOptions.remotePrincipalId = remotePrincipalIdOption->as<std::string>();
        authenticationOptions.remoteProfile = remoteScopeProfileOption->as<std::string>();
        return authenticationOptions;
    }

    std::string ReferenceAuthenticationConfiguration::bearerTokenFile() const {
        return bearerTokenFileOption->as<std::string>();
    }

    bool ReferenceAuthenticationConfiguration::verifiedLocalTrustEnabled() const {
        return verifiedLocalTrustOption->as<bool>();
    }

    bool ReferenceAuthenticationConfiguration::insecureLocalTrustOverride() const {
        return insecureLocalTrustOverrideOption->as<bool>();
    }

    std::string defaultSocketPath() {
        const char* runtimeDirectory = std::getenv("XDG_RUNTIME_DIR");
        if (runtimeDirectory != nullptr && *runtimeDirectory != '\0') {
            return (std::filesystem::path(runtimeDirectory) / "snodec-codex-backend.sock").string();
        }

        return (std::filesystem::path("/tmp") / ("snodec-codex-backend-" + std::to_string(::getuid()) + ".sock")).string();
    }

} // namespace apps::codex_backend
