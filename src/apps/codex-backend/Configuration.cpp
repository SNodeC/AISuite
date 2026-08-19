/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/Configuration.h"

#include "apps/codex-backend/FrontendWebSecurity.h"
#include "apps/codex-backend/ReferenceInvocationPolicy.h"
#include "utils/Config.h"

#include <arpa/inet.h>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace apps::codex_backend {

    namespace {

        CLI::Option* configurableBoolean(std::string name, std::string description, bool defaultValue) {
            auto& root = utils::Config::configRoot;
            const std::string defaultText = defaultValue ? "true" : "false";
            return root.setConfigurable(
                root.addFlag(std::move(name) + "{true}", std::move(description), "BOOL", defaultText, CLI::IsMember({"true", "false"})),
                true);
        }

        CLI::Option* configurableString(std::string name, std::string description, std::string defaultValue, std::string type) {
            auto& root = utils::Config::configRoot;
            return root.setConfigurable(
                root.addOption(std::move(name), std::move(description), std::move(type), std::move(defaultValue), CLI::Validator{}), true);
        }

        template <typename Value>
        CLI::Option* configurableUnsigned(std::string name, std::string description, std::string type, Value defaultValue) {
            auto& root = utils::Config::configRoot;
            return root.setConfigurable(
                root.addOption(std::move(name), std::move(description), std::move(type), defaultValue, CLI::TypeValidator<Value>()), true);
        }

        std::vector<std::string> commaSeparatedValues(std::string_view value) {
            std::vector<std::string> values;
            if (value.empty()) {
                return values;
            }
            std::size_t begin = 0;
            while (begin <= value.size()) {
                const std::size_t end = value.find(',', begin);
                values.emplace_back(value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin));
                if (end == std::string_view::npos) {
                    break;
                }
                begin = end + 1;
            }
            return values;
        }

    } // namespace

    AppServerProcessConfiguration::AppServerProcessConfiguration() {
        codexHomeOption = configurableString("--codex-home", "Set CODEX_HOME for the spawned Codex app-server process", {}, "PATH");
    }

    std::vector<std::pair<std::string, std::string>> AppServerProcessConfiguration::environmentOverrides() const {
        const std::string codexHome = codexHomeOption->as<std::string>();
        if (codexHome.empty()) {
            return {};
        }
        return {{"CODEX_HOME", codexHome}};
    }

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
            root.setConfigurable(root.addFlag("--frontend-unix-insecure-local-trust{true}",
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
                                                                       CLI::IsMember({"default_remote", "local_trusted"})),
                                                        true);
        allowInsecureRemoteOption =
            configurableBoolean("--frontend-allow-insecure-remote",
                                "Allow authenticated plaintext frontend listeners on non-loopback addresses (unsafe)",
                                false);
    }

    ReferenceAuthenticationOptions ReferenceAuthenticationConfiguration::options() const {
        ReferenceAuthenticationOptions authenticationOptions = defaultReferenceAuthenticationOptions();
        authenticationOptions.remotePrincipalId = remotePrincipalIdOption->as<std::string>();
        authenticationOptions.remoteProfile = remoteScopeProfileOption->as<std::string>();
        if (const auto scopes = referenceScopesForProfile(authenticationOptions.remoteProfile)) {
            authenticationOptions.remoteScopes = *scopes;
        } else {
            authenticationOptions.remoteScopes.clear();
        }
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

    bool ReferenceAuthenticationConfiguration::allowInsecureRemote() const {
        return allowInsecureRemoteOption->as<bool>();
    }

    std::optional<std::string> FrontendWebOptions::validationError() const {
        if (!normalizedFrontendWebSocketEndpoint(endpoint)) {
            return "the WebSocket frontend endpoint must be an exact normalized absolute path without a query or fragment";
        }
        if (staticRoot) {
            std::error_code error;
            const std::filesystem::path canonical = std::filesystem::canonical(*staticRoot, error);
            if (error || !std::filesystem::is_directory(canonical, error) || error || canonical == canonical.root_path()) {
                return "the configured frontend static root must be an existing canonical directory";
            }
        }
        for (const std::string& origin : allowedOrigins) {
            if (!normalizeWebOrigin(origin).has_value()) {
                return "the frontend Origin allow-list must contain only normalized HTTP or HTTPS origins without wildcards";
            }
        }

        return std::nullopt;
    }

    FrontendWebConfiguration::FrontendWebConfiguration() {
        endpointOption = configurableString(
            "--frontend-websocket-endpoint", "Exact HTTP request path used for frontend WebSocket upgrades", "/frontend", "PATH");
        staticRootOption = configurableString("--frontend-static-root", "Optional canonical root for static frontend assets", {}, "PATH");
        allowedOriginsOption = configurableString("--frontend-websocket-allowed-origins",
                                                  "Comma-separated explicit browser Origin allow-list (wildcards are forbidden)",
                                                  {},
                                                  "ORIGINS");
    }

    FrontendWebOptions FrontendWebConfiguration::options() const {
        FrontendWebOptions webOptions{
            .endpoint = endpointOption->as<std::string>(),
            .staticRoot = std::nullopt,
            .allowedOrigins = commaSeparatedValues(allowedOriginsOption->as<std::string>()),
        };
        const std::string configuredStaticRoot = staticRootOption->as<std::string>();
        if (!configuredStaticRoot.empty()) {
            std::error_code error;
            const std::filesystem::path canonical = std::filesystem::canonical(configuredStaticRoot, error);
            webOptions.staticRoot = error ? std::filesystem::path(configuredStaticRoot) : canonical;
        }
        return webOptions;
    }

    FrontendRuntimeConfiguration::FrontendRuntimeConfiguration() {
        filesystemReadOption =
            configurableBoolean("--frontend-filesystem-read-enabled", "Enable provider filesystem read/search/watch methods", false);
        filesystemWriteOption =
            configurableBoolean("--frontend-filesystem-write-enabled", "Enable provider filesystem mutation methods", false);
        filesystemRootOption = configurableString(
            "--frontend-filesystem-root", "Canonical root permitted for enabled provider filesystem methods", {}, "PATH");
        commandExecutionOption =
            configurableBoolean("--frontend-command-execution-enabled", "Enable provider command-execution methods", false);
        commandExecutableOption = configurableString(
            "--frontend-command-executable", "Exact argv[0] permitted for command.exec when command execution is enabled", {}, "PATH");
        shellCommandOption = configurableString(
            "--frontend-shell-command", "Exact thread.shellCommand program permitted when command execution is enabled", {}, "COMMAND");
        maxConnectionsOption =
            configurableUnsigned<std::size_t>("--frontend-max-connections", "Maximum frontend connections", "COUNT", 128);
        maxUnauthenticatedConnectionsOption = configurableUnsigned<std::size_t>(
            "--frontend-max-unauthenticated-connections", "Maximum unauthenticated frontend connections", "COUNT", 16);
        handshakeTimeoutOption = configurableUnsigned<std::uint64_t>(
            "--frontend-handshake-timeout-ms", "Frontend authentication handshake timeout", "MILLISECONDS", 10000);
        maximumInboundMessageBytesOption = configurableUnsigned<std::size_t>(
            "--frontend-maximum-message-bytes",
            "Maximum inbound frontend message bytes",
            "BYTES",
            ai::openai::codex::frontend::DefaultFrontendMaximumInboundMessageBytes);
        maxInboundMessagesPerSecondOption =
            configurableUnsigned<std::size_t>("--frontend-max-messages-per-second", "Sustained inbound frontend message rate", "COUNT", 50);
        maxInboundBurstOption =
            configurableUnsigned<std::size_t>("--frontend-max-message-burst", "Maximum inbound frontend message burst", "COUNT", 100);
        maxOutstandingCommandsOption = configurableUnsigned<std::size_t>(
            "--frontend-max-outstanding-commands", "Maximum outstanding commands per frontend connection", "COUNT", 256);
        maximumFailedAuthenticationsOption = configurableUnsigned<std::size_t>(
            "--frontend-max-failed-authentications-per-peer", "Failed authentications admitted per peer window", "COUNT", 3);
        failedAuthenticationWindowOption = configurableUnsigned<std::uint64_t>(
            "--frontend-failed-authentication-window-ms", "Failed-authentication peer window", "MILLISECONDS", 60000);
    }

    std::optional<std::string> FrontendRuntimeConfiguration::apply(ai::openai::codex::frontend::FrontendServiceOptions& options) const {
        options.enableFilesystemReadMethods = filesystemReadOption->as<bool>();
        options.enableFilesystemWriteMethods = filesystemWriteOption->as<bool>();
        options.enableCommandExecutionMethods = commandExecutionOption->as<bool>();
        options.maxConnections = maxConnectionsOption->as<std::size_t>();
        options.maxUnauthenticatedConnections = maxUnauthenticatedConnectionsOption->as<std::size_t>();
        options.handshakeTimeoutMs = handshakeTimeoutOption->as<std::uint64_t>();
        options.maximumInboundMessageBytes = maximumInboundMessageBytesOption->as<std::size_t>();
        options.maxInboundMessagesPerSecond = maxInboundMessagesPerSecondOption->as<std::size_t>();
        options.maxInboundBurst = maxInboundBurstOption->as<std::size_t>();
        options.maxOutstandingCommandsPerConnection = maxOutstandingCommandsOption->as<std::size_t>();
        options.maximumFailedAuthenticationsPerPeer = maximumFailedAuthenticationsOption->as<std::size_t>();
        options.failedAuthenticationWindowMs = failedAuthenticationWindowOption->as<std::uint64_t>();

        const std::string filesystemRoot = filesystemRootOption->as<std::string>();
        const std::string commandExecutable = commandExecutableOption->as<std::string>();
        const std::string shellCommand = shellCommandOption->as<std::string>();
        if ((options.enableFilesystemReadMethods || options.enableFilesystemWriteMethods) && filesystemRoot.empty()) {
            return "enabled filesystem frontend methods require --frontend-filesystem-root";
        }
        if (options.enableCommandExecutionMethods && commandExecutable.empty() && shellCommand.empty()) {
            return "enabled command-execution frontend methods require an exact executable or shell-command allowlist entry";
        }

        ReferenceInvocationPolicyOptions policyOptions;
        if (!filesystemRoot.empty()) {
            policyOptions.filesystemRoots.emplace_back(filesystemRoot);
        }
        if (!commandExecutable.empty()) {
            policyOptions.commands.executables.push_back(commandExecutable);
        }
        if (!shellCommand.empty()) {
            policyOptions.commands.shellCommands.push_back(shellCommand);
        }
        std::shared_ptr<ReferenceInvocationPolicy> policy;
        try {
            policy = std::make_shared<ReferenceInvocationPolicy>(std::move(policyOptions));
        } catch (const std::exception&) {
            return "frontend filesystem or command allowlist policy is invalid";
        }

        if (options.enableFilesystemReadMethods) {
            options.filesystemReadPolicy = [policy](const auto&, std::string_view method, const auto& parameters) {
                return policy->allowsFilesystemRead(method, parameters);
            };
        }
        if (options.enableFilesystemWriteMethods) {
            options.filesystemWritePolicy = [policy](const auto&, std::string_view method, const auto& parameters) {
                return policy->allowsFilesystemWrite(method, parameters);
            };
        }
        if (options.enableCommandExecutionMethods) {
            options.commandExecutionPolicy = [policy](const auto&, std::string_view method, const auto& parameters) {
                return policy->allowsCommandExecution(method, parameters);
            };
        }
        return std::nullopt;
    }

    bool isLoopbackFrontendAddress(std::string_view address, bool ipv6) noexcept {
        // Plaintext listener admission is deliberately limited to numeric
        // addresses so DNS aliases cannot change meaning between validation
        // and bind. The bind callback repeats this check against SNode.C's
        // effective numeric socket address.
        try {
            const std::string text(address);
            if (ipv6) {
                in6_addr parsed{};
                return ::inet_pton(AF_INET6, text.c_str(), &parsed) == 1 && IN6_IS_ADDR_LOOPBACK(&parsed) != 0;
            }
            in_addr parsed{};
            return ::inet_pton(AF_INET, text.c_str(), &parsed) == 1 && (ntohl(parsed.s_addr) & 0xFF000000U) == 0x7F000000U;
        } catch (...) {
            return false;
        }
    }

    std::string defaultSocketPath() {
        const char* runtimeDirectory = std::getenv("XDG_RUNTIME_DIR");
        if (runtimeDirectory != nullptr && *runtimeDirectory != '\0') {
            return (std::filesystem::path(runtimeDirectory) / "snodec-codex-backend.sock").string();
        }

        return (std::filesystem::path("/tmp") / ("snodec-codex-backend-" + std::to_string(::getuid()) + ".sock")).string();
    }

} // namespace apps::codex_backend
